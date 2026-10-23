/*
 * ui_zwave.c - Installer > Z-Wave (docs/features/36 screens.md): the installer menu's row (§2)
 * and the Z-Wave page (§3) with the radio, the devices and the network's settings. The pages it
 * leads to - adding a device (§4), a device's page (§6), removing (§7), the region and the
 * radio's details (§8) - slot into PAGES below; a row whose page isn't there yet shows no
 * chevron and does nothing.
 *
 * Everything comes through ui_backend_t's zw_* calls as states and numbers, and the words are
 * here - except a model's name and how to wake it (the quirks table's) and an input's words (the
 * zone source's, which the source picker shows too). Live words are read again once a second
 * and change only when they differ (UI-020, UI-023): rows stay where they are and only their
 * words change; the device list is made again only when its devices or their order change.
 *
 * Compiled only with ALARM_UI_ZWAVE (sources.cmake).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define POLL_MS 1000u

/* ---- the backend ----------------------------------------------------------------------------- */

/* The page whose operation runs (add or remove), and what it started: leaving that page - and
 * only that page - stops it or clears its result (stop_if_leaving). 0: none of ours. An operation
 * the console started, or one these pages did not open, is never stopped by a page change, and a
 * result from before is never shown as this visit's. 0 (PAGE_DISPLAY, never an owner): none. */
static page_id_t s_owner;

static bool read_status(ui_zw_status_t *st)
{
    memset(st, 0, sizeof(*st));
    return S.be.zw_status != NULL && S.be.zw_status(S.be.ctx, st);
}

bool ui_zwave_available(void)
{
    ui_zw_status_t st;
    return read_status(&st);
}

static void read_progress(ui_zw_progress_t *p)
{
    memset(p, 0, sizeof(*p));
    if (S.be.zw_progress != NULL) {
        S.be.zw_progress(S.be.ctx, p);
    }
}

/* The shape a step belongs to (add and remove share it): a rebuild is needed only when this
 * changes (UI-020, UI-023) - within one family, a tick updates words in place. */
typedef enum { ZWF_LISTEN, ZWF_ALLOW, ZWF_PIN, ZWF_PROGRESS, ZWF_DONE } zw_step_family_t;

static zw_step_family_t step_family(const ui_zw_progress_t *p)
{
    switch (p->step) {
    case UI_ZW_STEP_ASK_ALLOW: return ZWF_ALLOW;
    case UI_ZW_STEP_ASK_PIN: return ZWF_PIN;
    case UI_ZW_STEP_CONNECTING:
    case UI_ZW_STEP_SECURING:
    case UI_ZW_STEP_READING: return ZWF_PROGRESS;
    case UI_ZW_STEP_DONE: return ZWF_DONE;
    default: return ZWF_LISTEN; /* idle, starting, listening */
    }
}

static const char *request_refused(ui_zw_req_result_t r)
{
    switch (r) {
    /* TR: a toast, why the Z-Wave radio refused a request */
    case UI_ZW_REQ_NOT_ALLOWED: return TR("Only in installer mode, with everything disarmed");
    /* TR: a toast, why the Z-Wave radio refused a request */
    case UI_ZW_REQ_BUSY: return TR("The radio is busy: try again when it's done");
    /* TR: a toast, why the Z-Wave radio refused a request */
    case UI_ZW_REQ_NO_RADIO: return TR("The Z-Wave radio isn't working");
    /* TR: a toast, why the Z-Wave radio refused a request */
    case UI_ZW_REQ_UNSUPPORTED: return TR("This panel can't do that yet");
    /* TR: a toast, why the Z-Wave radio refused a request */
    case UI_ZW_REQ_LOW_BATTERY: return TR("Its battery is too low for an update: replace it first");
    /* TR: a toast, why the Z-Wave radio refused a request (ZW-039: Zigbee's words) */
    case UI_ZW_REQ_FULL: return TR("No room for another device: the panel's storage is full");
    /* TR: a toast, why the Z-Wave radio refused a request */
    default: return TR("That can't be done now");
    }
}

/* The same reasons as whole sentences for a page's paragraph (each says "Not started: ..."). */
static const char *request_not_started(ui_zw_req_result_t r)
{
    switch (r) {
    /* TR: a paragraph on the Add or Remove page, why nothing started */
    case UI_ZW_REQ_NOT_ALLOWED:
        /* TR: a paragraph on the Add or Remove page, why nothing started */
        return TR("Not started: only in installer mode, with everything disarmed.");
    /* TR: a paragraph on the Add or Remove page, why nothing started */
    case UI_ZW_REQ_BUSY: return TR("Not started: the radio is busy: try again when it's done.");
    /* TR: a paragraph on the Add or Remove page, why nothing started */
    case UI_ZW_REQ_NO_RADIO: return TR("Not started: the Z-Wave radio isn't working.");
    /* TR: a paragraph on the Add or Remove page, why nothing started */
    case UI_ZW_REQ_UNSUPPORTED: return TR("Not started: this panel can't do that yet.");
    /* TR: a paragraph on the Add or Remove page, why nothing started */
    case UI_ZW_REQ_LOW_BATTERY:
        /* TR: a paragraph on the Add or Remove page, why nothing started */
        return TR("Not started: its battery is too low for an update: replace it first.");
    case UI_ZW_REQ_FULL:
        /* TR: a paragraph on the Add page, why nothing started (ZW-039: Zigbee's words) */
        return TR("Not started: there is no room for another device. Remove one you no longer "
                  "use first.");
    /* TR: a paragraph on the Add or Remove page, why nothing started */
    default: return TR("Not started: that can't be done now.");
    }
}

/* Why an add or a removal didn't start, on its page (a toast is gone before it is read; B015
 * UX-029): a device's software update keeps the network busy, and says so. `adding` picks the
 * whole sentence. */
static void refused_paragraph(lv_obj_t *body, ui_zw_req_result_t r, bool adding)
{
    ui_zw_progress_t p;
    memset(&p, 0, sizeof(p));
    if (S.be.zw_progress != NULL) {
        S.be.zw_progress(S.be.ctx, &p);
    }
    char text[160];
    if (r == UI_ZW_REQ_BUSY && p.op == UI_ZW_OP_FIRMWARE && p.step != UI_ZW_STEP_DONE) {
        /* TR: a paragraph on the Add a device page: the update keeps the radio busy */
        i18n_utf8_copy(text, sizeof(text),
                       adding ? TR("A device is getting new software. Add a device when it's done.")
                              : TR("A device is getting new software. Remove a device when it's "
                                   "done."));
    } else {
        i18n_utf8_copy(text, sizeof(text), request_not_started(r));
    }
    ui_paragraph(body, text, ui_tone_amber());
}

/* A request; its refusal is said in a toast. */
static bool request(const ui_zw_request_t *req)
{
    if (S.be.zw_request == NULL) {
        return false;
    }
    const ui_zw_req_result_t r = S.be.zw_request(S.be.ctx, req);
    if (r != UI_ZW_REQ_OK) {
        ui_toast(request_refused(r));
        return false;
    }
    return true;
}

/* ---- words (the pages to come use them too) -------------------------------------------------- */

/* The region codes of docs/features/36 serial-api.md §6.7, in the words of screens.md §8. */
static const char *region_name(uint8_t code)
{
    switch (code) {
    /* TR: a Z-Wave region (radio frequency band), a row's value and a choice. At most 31 bytes */
    case 0x00u: return TR_C("Z-Wave region", "Europe");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x01u: return TR_C("Z-Wave region", "United States");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x02u: return TR_C("Z-Wave region", "Australia and New Zealand");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x03u: return TR_C("Z-Wave region", "Hong Kong");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x05u: return TR_C("Z-Wave region", "India");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x06u: return TR_C("Z-Wave region", "Israel");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x07u: return TR_C("Z-Wave region", "Russia");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x08u: return TR_C("Z-Wave region", "China");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x09u: return TR_C("Z-Wave region", "United States with Long Range");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x0Bu: return TR_C("Z-Wave region", "Europe with Long Range");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x20u: return TR_C("Z-Wave region", "Japan");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    case 0x21u: return TR_C("Z-Wave region", "Korea");
    /* TR: a Z-Wave region, a row's value and a choice. At most 31 bytes */
    default: return TR_C("Z-Wave region", "Unknown");
    }
}

/* "Every hour", "Every 4 hours", "Every 30 min", "Never". */
static void check_in_words(uint32_t s, char *out, size_t len)
{
    /* TR: how often a Z-Wave device reports in, a row's value or a choice. At most 23 bytes */
    if (s == 0u) {
        i18n_utf8_copy(out, len, TR_C("check-in", "Never"));
    } else if (s == 3600u) {
        /* TR: how often a Z-Wave device reports in. At most 23 bytes */
        i18n_utf8_copy(out, len, TR("Every hour"));
    } else if (s % 3600u == 0u) {
        /* TR: how often a Z-Wave device reports in; {0} is 2 to 12. At most 16 bytes */
        I18N_FORMAT(out, len, TRN("Every {0} hours", "Every {0} hours", s / 3600u),
                    (unsigned)(s / 3600u));
    } else {
        /* TR: how often a Z-Wave device reports in; {0} is 1 to 59. At most 16 bytes */
        I18N_FORMAT(out, len, TRN("Every {0} min", "Every {0} min", (s + 59u) / 60u),
                    (unsigned)((s + 59u) / 60u));
    }
}

/* "No devices", "1 device", "5 devices". */
static void count_words(unsigned n, char *out, size_t len)
{
    if (n == 0u) {
        /* TR: the number of Z-Wave devices, in a row. At most 20 bytes */
        i18n_utf8_copy(out, len, TR("No devices"));
    } else {
        /* TR: the number of Z-Wave devices, in a row. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} device", "{0} devices", n), n);
    }
}

/* ZW-040: a device page's Signal row (the Zigbee page's words); never called for UNKNOWN. */
static const char *signal_words(uint8_t signal)
{
    switch (signal) {
    case UI_ZW_SIGNAL_GOOD:
        /* TR: a Z-Wave device page's Signal row. At most 31 bytes */
        return TR_C("signal", "Good");
    case UI_ZW_SIGNAL_FAIR:
        /* TR: a Z-Wave device page's Signal row. At most 31 bytes */
        return TR_C("signal", "Fair");
    default:
        /* TR: a Z-Wave device page's Signal row. At most 31 bytes */
        return TR_C("signal", "Weak");
    }
}

/* How long, in the words of "Not heard for ...": "45 min", "2 h 30 min", "3 days". */
static void span_words(uint32_t s, char *out, size_t len)
{
    const uint32_t min = s / 60u;
    if (min < 60u) {
        /* TR: a span of time, inserted into "Not heard for {0}", "{0} ago", "Reported {0} ago" and
           "Heard {0} ago" (so it must read well in all of them). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} min", "{0} min", min == 0u ? 1u : min),
                    (unsigned)(min == 0u ? 1u : min));
    } else if (min < 48u * 60u) {
        if (min % 60u == 0u) {
            /* TR: how long a device has been silent (hours). At most 18 bytes */
            I18N_FORMAT(out, len, TRN("{0} h", "{0} h", min / 60u), (unsigned)(min / 60u));
        } else {
            /* TR: how long a device has been silent: {0} hours {1} minutes. At most 18 bytes */
            I18N_FORMAT(out, len, TRN("{0} h {1} min", "{0} h {1} min", min % 60u),
                        (unsigned)(min / 60u), (unsigned)(min % 60u));
        }
    } else {
        /* TR: how long a device has been silent (days, 2 or more). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} days", "{0} days", min / (24u * 60u)),
                    (unsigned)(min / (24u * 60u)));
    }
}

/* Exported for the zone page's "Z-Wave device" row (ui_sources.c, screens.md §5). */
void ui_zwave_span_words(uint32_t s, char *out, size_t len)
{
    span_words(s, out, len);
}

/* A model's short form, its first two words - the maker and the model number, "Zooz ZSE41" of
 * "Zooz ZSE41 open/close sensor" (screens.md §3, §5) - where a line has no room for the whole
 * name; "" when the quirks table doesn't name it. */
void ui_zwave_short_model(const char *model, char *out, size_t len)
{
    const char *space = strchr(model, ' ');
    const char *end = space != NULL ? strchr(space + 1, ' ') : NULL;
    const int n = end != NULL ? (int)(end - model) : (int)strlen(model);
    snprintf(out, len, "%.*s", n, model);
}

/* The list's short words for a device's security; "" when not known yet. */
static const char *security_short(uint8_t sec)
{
    switch (sec) {
    case UI_ZW_SEC_S2_UNAUTH:
    case UI_ZW_SEC_S2_AUTH:
    /* TR: a device's security, short, in its row of the device list */
    case UI_ZW_SEC_S2_ACCESS: return TR_C("device security", "Secure");
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_S0: return TR_C("device security", "Older security");
    case UI_ZW_SEC_NONE:
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_NONE_CAPABLE: return TR_C("device security", "Not encrypted");
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_UNCONFIRMED: return TR_C("device security", "Security not confirmed");
    default: return "";
    }
}

/* Its security is a warning: not encrypted, or not confirmed to be. */
static bool security_warns(uint8_t sec)
{
    return sec == UI_ZW_SEC_NONE || sec == UI_ZW_SEC_NONE_CAPABLE ||
           sec == UI_ZW_SEC_UNCONFIRMED;
}

/* Zone names: the installer's working copy while it is being edited. */
static const alarm_config_t *zone_cfg(void)
{
    return S.program && S.edit != NULL ? S.edit : S.cfg;
}

/* What a device is called: its (first) zone's name, else its model. */
/* "Z-Wave keypad", "Z-Wave switch"...: a device without a model name (B015 UX-038, UX-056). */
static const char *kind_words(uint8_t kind)
{
    /* TR: what a Z-Wave device is, on its page's note line when it has no model name */
    static const char *const K[] = {
        TR_NOOP_C("Z-Wave device kind", "Z-Wave device"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave sensor"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave keypad"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave switch"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave dimmer"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave relays"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave siren"),
        TR_NOOP_C("Z-Wave device kind", "Z-Wave range extender")};
    return TR_C("Z-Wave device kind", kind < sizeof(K) / sizeof(K[0]) ? K[kind] : K[0]);
}

static void device_name(const ui_zw_device_t *d, char *out, size_t len)
{
    if (d->zone_count > 0u) {
        ui_zone_name(zone_cfg(), d->zones[0], out, len);
    } else if (d->model[0] != '\0') {
        snprintf(out, len, "%s", d->model);
    } else {
        /* no model known: what it is, from what it lists (B015 UX-009) */
        /* TR: what a Z-Wave device is called in the device list when it has no model name and
         * no zone. At most 47 bytes */
        static const char *const KIND[] = {
            TR_NOOP_C("device without a name", "Unknown device"),
            TR_NOOP_C("device without a name", "Sensor"),
            TR_NOOP_C("device without a name", "Keypad"),
            TR_NOOP_C("device without a name", "Switch"),
            TR_NOOP_C("device without a name", "Dimmer"),
            TR_NOOP_C("device without a name", "Relays"),
            TR_NOOP_C("device without a name", "Siren"),
            TR_NOOP_C("device without a name", "Range extender")};
        i18n_utf8_copy(out, len,
                       TR_C("device without a name",
                            d->kind < sizeof(KIND) / sizeof(KIND[0]) ? KIND[d->kind] : KIND[0]));
    }
}

/* "Zone 3", "Zones 3, 4", "Not in a zone". */
static void zones_words(const ui_zw_device_t *d, char *out, size_t len)
{
    if (d->zone_count == 0u) {
        /* a keypad, a switch... is never a zone: nothing to say (B015 UX-038) */
        /* TR: a Z-Wave device's row in the device list, its zones. At most 31 bytes */
        i18n_utf8_copy(out, len,
                       d->kind == UI_ZW_KIND_SENSOR || d->kind == UI_ZW_KIND_DEVICE
                           ? TR("Not in a zone")
                           : "");
        return;
    }
    char list[40]; /* the zone numbers: "3, 4, 5" */
    snprintf(list, sizeof(list), "%u", (unsigned)d->zones[0]);
    for (size_t i = 1; i < d->zone_count && i < UI_ZW_ZONES_MAX; i++) {
        char one[8];
        char joined[40];
        snprintf(one, sizeof(one), "%u", (unsigned)d->zones[i]);
        /* TR: a list of names or numbers: {0} what is listed so far, {1} the next one */
        I18N_FORMAT(joined, sizeof(joined), TR_C("list of names", "{0}, {1}"), list, one);
        i18n_utf8_copy(list, sizeof(list), joined);
    }
    /* TR: a Z-Wave device's row in the device list, its zone numbers ({0}: "3" or "3, 4").
     * At most 25 bytes */
    I18N_FORMAT(out, len, TRN("Zone {0}", "Zones {0}", d->zone_count), list);
}

/* Appends " · part" (or "part" to an empty line). */
static void join(char *out, size_t len, const char *part)
{
    const size_t n = strlen(out);
    if (part[0] != '\0' && n < len) {
        snprintf(out + n, len - n, "%s%s", n > 0u ? " · " : "", part);
    }
}

/* Appends "-part" (or "part" to an empty line): the DSK's groups (screens.md §4 PIN page). */
static void dash_join(char *out, size_t len, const char *part)
{
    const size_t n = strlen(out);
    if (part[0] != '\0' && n < len) {
        snprintf(out + n, len - n, "%s%s", n > 0u ? "-" : "", part);
    }
}

/* Splits a DSK ("12345-23456-...", 8 groups of 5 digits, the first the PIN) into the two lines
 * of groups 2-5 and 6-8 that the PIN page shows to check against the label (screens.md §4). */
static void dsk_rest(const char *dsk, char *line1, size_t l1, char *line2, size_t l2)
{
    char buf[UI_ZW_DSK_LEN];
    snprintf(buf, sizeof(buf), "%s", dsk);
    line1[0] = '\0';
    line2[0] = '\0';
    size_t i = 0;
    for (char *tok = strtok(buf, "-"); tok != NULL; tok = strtok(NULL, "-"), i++) {
        if (i == 0u) {
            continue; /* the PIN, already shown on its own */
        }
        dash_join(i <= 4u ? line1 : line2, i <= 4u ? l1 : l2, tok);
    }
}

/* "every hour" / "every 4 hours" / "every 30 min", for a sentence (screens.md §4, §5); "" when
 * the device never checks in (mains powered). */
static void checkin_sentence(uint32_t s, char *out, size_t len)
{
    out[0] = '\0';
    if (s == 0u) {
        return;
    }
    /* TR: how often a device reports in, as a value inside a longer sentence. At most 23 bytes */
    if (s == 3600u) {
        i18n_utf8_copy(out, len, TR_C("check-in inside a sentence", "every hour"));
    } else if (s % 3600u == 0u) {
        /* TR: how often a device reports in, inside a longer sentence; {0} is 2 to 12. At most
         * 16 bytes */
        I18N_FORMAT(out, len,
                    TRN_C("check-in inside a sentence", "every {0} hours", "every {0} hours",
                          s / 3600u),
                    (unsigned)(s / 3600u));
    } else {
        /* TR: how often a device reports in, inside a longer sentence; {0} is 1 to 59. At most
         * 16 bytes */
        I18N_FORMAT(out, len,
                    TRN_C("check-in inside a sentence", "every {0} min", "every {0} min",
                          (s + 59u) / 60u),
                    (unsigned)((s + 59u) / 60u));
    }
}

/* The device page's and the results page's full security words (screens.md §4, §6). */
static const char *security_full(uint8_t sec)
{
    switch (sec) {
    /* TR: a device's security in full, a row's value on its page */
    case UI_ZW_SEC_S2_ACCESS: return TR_C("device security", "Access control");
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_S2_AUTH: return TR_C("device security", "With its PIN");
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_S2_UNAUTH: return TR_C("device security", "Without a PIN");
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_S0: return TR_C("device security", "Older security (S0)");
    case UI_ZW_SEC_NONE: return TR_C("device security", "Not encrypted");
    case UI_ZW_SEC_NONE_CAPABLE:
        /* TR: a device's security, a row's value on its page */
        return TR_C("device security", "Not encrypted (it supports encryption)");
    /* TR: a device's security, a row's value on its page */
    case UI_ZW_SEC_UNCONFIRMED: return TR_C("device security", "Not confirmed: its setup didn't finish");
    /* TR: a device's security, a row's value on its page */
    default: return TR_C("device security", "Not known yet");
    }
}

/* screens.md §8 radio details: State. */
static const char *radio_state_word(uint8_t radio)
{
    switch (radio) {
    /* TR: the state of the Z-Wave radio, a row's value; also after "Radio · " in the page's
     * first row. At most 31 bytes */
    case UI_ZW_RADIO_WORKING: return TR_C("radio state", "Working");
    /* TR: the state of the Z-Wave radio, a row's value */
    case UI_ZW_RADIO_STARTING: return TR_C("radio state", "Starting");
    /* TR: the state of the Z-Wave radio, a row's value */
    case UI_ZW_RADIO_NOT_ANSWERING: return TR_C("radio state", "Not answering");
    /* TR: the state of the Z-Wave radio, a row's value */
    case UI_ZW_RADIO_UNUSABLE: return TR_C("radio state", "Can't be used");
    /* TR: the state of the Z-Wave radio, a row's value */
    case UI_ZW_RADIO_FOREIGN: return TR_C("radio state", "Belongs to another system");
    /* TR: the state of the Z-Wave radio, a row's value */
    case UI_ZW_RADIO_REPLACED: return TR_C("radio state", "Replaced or reset");
    /* TR: the state of the Z-Wave radio, a row's value */
    case UI_ZW_RADIO_UPDATING: return TR_C("radio state", "Updating");
    /* TR: the state of the Z-Wave radio, a row's value */
    default: return TR_C("radio state", "Not connected");
    }
}

/* Installer mode only (screens.md "Who"; installer mode keeps every partition disarmed). */
typedef enum { LOCKED_ADD, LOCKED_REMOVE, LOCKED_REGION } locked_what_t;

static void locked_paragraph(lv_obj_t *body, locked_what_t what)
{
    const char *text;
    switch (what) {
    /* TR: a paragraph on the Add a device page, for someone who is not in installer mode */
    case LOCKED_ADD: text = TR("Adding a device is done in installer mode."); break;
    /* TR: a paragraph on the Remove a device page, for someone who is not in installer mode */
    case LOCKED_REMOVE: text = TR("Removing a device is done in installer mode."); break;
    /* TR: a paragraph on the Z-Wave region page, for someone who is not in installer mode */
    default: text = TR("Changing the region is done in installer mode."); break;
    }
    ui_paragraph(body, text, ui_tone_gray());
}

static void unavailable_paragraph(lv_obj_t *body)
{
    /* TR: a row or paragraph on a Z-Wave page */
    ui_paragraph(body, TR("Z-Wave isn't available on this panel."), ui_tone_gray());
}

/* ---- §5: putting a device's input to work (also used by the add flow's "Use it for") --------- */

/* Stable storage for the inputs shown on screen: row callbacks get an index, never a pointer into
 * a page struct that a later tick has already overwritten. */
static ui_zw_input_t s_use_inputs[UI_ZW_INPUTS_MAX] UI_SLOW_BSS; /* PSRAM (B001) */
static char s_use_titles[UI_ZW_INPUTS_MAX][48] UI_SLOW_BSS; /* its words as a title, not a
                                                 * mid-sentence detail (screens.md §4) */
static size_t s_use_n;
static uint16_t s_use_node, s_use_generation;

/* What a pending ui_confirm() dialog acts on, when it isn't a fixed callback (no args of its
 * own): set just before ui_confirm(), read back by the confirmed function. */
static void *s_confirm_arg;
/* PAGE_ZWAVE_ADD with arg 2 replaces this device (packed node | generation << 16; §10). */
static int32_t s_replace_arg;

static bool zw_free_zone(uint16_t *out)
{
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (S.edit->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
            *out = z;
            return true;
        }
    }
    return false;
}

static bool zw_bind_input(uint16_t zone, const ui_zw_input_t *in)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    b.in[ZSRC_ROLE_MAIN].addr = in->addr;
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, (alarm_device_class_t)in->device_class);
    if (S.be.zone_bind == NULL || S.be.zone_bind(S.be.ctx, zone, &b) != ZONE_HUB_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Not saved"));
        return false;
    }
    return true;
}

/* "New zone": a fresh zone of the input's suggested kind (screens.md §4 "Use it for"). */
static void use_new_zone(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i >= s_use_n) {
        return;
    }
    uint16_t z;
    if (!zw_free_zone(&z)) {
        ui_toast(TR("Every zone is in use"));
        return;
    }
    /* screens.md §4 "creates a zone with the suggested type" - a smoke or CO input must not
     * become a burglary zone. */
    ui_installer_create_zone_for(z, (alarm_device_class_t)s_use_inputs[i].device_class);
    if (!zw_bind_input(z, &s_use_inputs[i])) {
        return;
    }
    ui_installer_apply();
    ui_page_open(PAGE_ZONE, z);
}

static uint16_t s_pick_zones[ALARM_MAX_ZONES] UI_SLOW_BSS;
static size_t s_pick_n;
static size_t s_pick_input;    /* index into s_use_inputs */
static uint16_t s_pick_chosen; /* the zone chosen, once existing_zone_chosen ran */

static void existing_zone_confirmed(void)
{
    if (s_pick_input < s_use_n && zw_bind_input(s_pick_chosen, &s_use_inputs[s_pick_input])) {
        ui_page_open(PAGE_ZONE, s_pick_chosen);
    }
}

static void existing_zone_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_pick_n) {
        return;
    }
    s_pick_chosen = s_pick_zones[(size_t)index];
    char name[ALARM_NAME_LEN + 8];
    char title[96];
    /* TR: a confirmation's title; {0} is a zone's name. At most 64 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Replace {0}'s sensor?"),
                ui_zone_name(S.edit, s_pick_chosen, name, sizeof(name)));
    ui_confirm(title, TR("Its current sensor stops feeding this zone."),
               TR_C("confirm button", "Replace"), existing_zone_confirmed);
}

/* "Pick": choose one of the zones that already exist (screens.md §4 "Use it for"). */
static void use_existing_zone(void *ctx)
{
    s_pick_input = (size_t)(uintptr_t)ctx;
    if (s_pick_input >= s_use_n) {
        return;
    }
    static ui_choice_t choices[ALARM_MAX_ZONES] UI_SLOW_BSS; /* PSRAM: ~9 KB (B001) */
    static char titles[ALARM_MAX_ZONES][ALARM_NAME_LEN + 8] UI_SLOW_BSS;
    s_pick_n = 0;
    for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
        if (S.edit->zones[z - 1u].type == ALARM_ZT_NOT_USED) {
            continue;
        }
        s_pick_zones[s_pick_n] = z;
        ui_zone_name(S.edit, z, titles[s_pick_n], sizeof(titles[s_pick_n]));
        choices[s_pick_n].glyph = NULL;
        choices[s_pick_n].tone = ui_tone_blue();
        choices[s_pick_n].title = titles[s_pick_n];
        choices[s_pick_n].detail = NULL;
        s_pick_n++;
    }
    if (s_pick_n == 0u) {
        ui_toast(TR("No zones yet"));
        return;
    }
    ui_pick_sheet(TR("Pick a zone"), TR("Its current sensor is replaced"), choices, s_pick_n, -1,
                  existing_zone_chosen);
}

/* Fills s_use_inputs with the device's inputs that feed no zone yet. */
static size_t collect_use_inputs(uint16_t node, uint16_t generation)
{
    s_use_node = node;
    s_use_generation = generation;
    s_use_n = 0;
    ui_zw_device_page_t page;
    if (S.be.zw_device == NULL || !S.be.zw_device(S.be.ctx, node, generation, &page)) {
        return 0u;
    }
    for (uint8_t i = 0; i < page.input_count && s_use_n < UI_ZW_INPUTS_MAX; i++) {
        if (page.inputs[i].zone == 0u) {
            s_use_inputs[s_use_n] = page.inputs[i];
            /* its words as a title ("Door contact": screens.md §4/§6), in the screen's language */
            ui_zw_input_words(s_use_inputs[s_use_n].endpoint, s_use_inputs[s_use_n].slot,
                              s_use_inputs[s_use_n].device_class, true, s_use_titles[s_use_n],
                              sizeof(s_use_titles[s_use_n]));
            s_use_n++;
        }
    }
    return s_use_n;
}

/* One row per input the device offers, each two ways to use it (screens.md §4 "Use it for").
 * Returns the number of un-zoned inputs shown. */
static size_t render_use_it_for(lv_obj_t *body, uint16_t node, uint16_t generation)
{
    const size_t n = collect_use_inputs(node, generation);
    for (size_t i = 0; i < n; i++) {
        const ui_zw_input_t *in = &s_use_inputs[i];
        char detail[64];
        if (in->state[0] != '\0') {
            /* TR: under an input's name; {0} is what it reports now. At most 63 bytes */
            I18N_FORMAT(detail, sizeof(detail), TR("Right now: {0}"), ui_sensor_state_words(in->state));
        } else {
            /* TR: under an input's name, when it has reported nothing. At most 63 bytes */
            i18n_utf8_copy(detail, sizeof(detail), TR("Right now: nothing reported yet"));
        }
        ui_row_link(body, ui_device_class_glyph((alarm_device_class_t)in->device_class),
                    /* TR: a row's value or note where an input is put to work */
                    ui_tone_blue(), s_use_titles[i], detail, TR("New zone"),
                    UI_ICON_CHEVRON_RIGHT, use_new_zone, (void *)(uintptr_t)i);
        ui_row_link(body, ui_device_class_glyph((alarm_device_class_t)in->device_class),
                    /* TR: a row's value or note where an input is put to work */
                    ui_tone_gray(), s_use_titles[i], TR("For an existing zone"),
                    /* TR: a row's value or note where an input is put to work */
                    TR_C("button on a row", "Pick"), UI_ICON_CHEVRON_RIGHT, use_existing_zone,
                    (void *)(uintptr_t)i);
    }
    return n;
}

/* Device page's "Make it a zone" (screens.md §6 "Used by"): the one input if there's only one,
 * else a pick of which input to use - each still a fresh "New zone" (the fuller choice, "for an
 * existing zone", stays on the add flow's own results page). */
static void make_zone_for_input(int32_t index)
{
    if (index >= 0 && (size_t)index < s_use_n) {
        use_new_zone((void *)(uintptr_t)index);
    }
}

static void device_args(int32_t arg, uint16_t *node, uint16_t *generation)
{
    *node = (uint16_t)((uint32_t)arg & 0xFFFFu);
    *generation = (uint16_t)(((uint32_t)arg >> 16) & 0xFFFFu);
}

static void make_it_a_zone(void *ctx)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    if (collect_use_inputs(node, generation) == 0u) {
        ui_toast(TR("Nothing to use yet"));
        return;
    }
    if (s_use_n == 1u) {
        use_new_zone((void *)(uintptr_t)0);
        return;
    }
    static ui_choice_t choices[UI_ZW_INPUTS_MAX] UI_SLOW_BSS;
    for (size_t i = 0; i < s_use_n; i++) {
        choices[i].glyph = ui_device_class_glyph((alarm_device_class_t)s_use_inputs[i].device_class);
        choices[i].tone = ui_tone_blue();
        choices[i].title = s_use_titles[i];
        choices[i].detail = NULL;
    }
    ui_pick_sheet(TR("Pick what to use"), TR("Makes a new zone"), choices, s_use_n, -1,
                  make_zone_for_input);
}

/* ---- the pages ------------------------------------------------------------------------------- */

typedef void (*page_fn)(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen);

static void build_network(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                          size_t nlen);
static void build_add(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                      size_t nlen);
static void build_remove(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen);
static void build_device(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen);
static void build_region(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen);
static void build_radio(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen);
static void build_smart(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen);
static void build_smart_entry(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                              size_t nlen);
static void reset_row_tap(void *ctx);

/* The Z-Wave pages (Z7b: §4, §6, §7, §8). */
static const struct {
    page_id_t id;
    page_fn build;
} PAGES[] = {
    {PAGE_ZWAVE, build_network},
    {PAGE_ZWAVE_ADD, build_add},
    {PAGE_ZWAVE_REMOVE, build_remove},
    {PAGE_ZWAVE_DEVICE, build_device},
    {PAGE_ZWAVE_REGION, build_region},
    {PAGE_ZWAVE_RADIO, build_radio},
    {PAGE_ZWAVE_SMART, build_smart},
    {PAGE_ZWAVE_SMART_ENTRY, build_smart_entry},
};

static page_fn page_builder(page_id_t id)
{
    for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
        if (PAGES[i].id == id) {
            return PAGES[i].build;
        }
    }
    return NULL;
}

static void open_page(void *ctx)
{
    const page_id_t id = (page_id_t)(intptr_t)ctx;
    if (page_builder(id) != NULL) {
        ui_page_open(id, 0);
    }
}

/* A row that leads to a Z-Wave page: tappable, with its chevron, once that page exists. */
static lv_obj_t *page_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                          const char *detail, const char *value, page_id_t to)
{
    const bool there = page_builder(to) != NULL;
    return ui_row_link(body, glyph, tone, title, detail, value,
                       there ? UI_ICON_CHEVRON_RIGHT : NULL, there ? open_page : NULL,
                       (void *)(intptr_t)to);
}

/* Adding, removing, the region, the check-in, the reset: installer mode, every partition
 * disarmed (screens.md "Who"; installer mode keeps them disarmed). Anyone may look. */
static bool installer(void)
{
    return S.program;
}

/* A label's text and colour, set only when they differ (nothing is redrawn for nothing). */
static void set_words(lv_obj_t *row, const char *name, const char *text, lv_color_t color)
{
    lv_obj_t *l = ui_find(row, name);
    if (l == NULL) {
        return;
    }
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
    lv_obj_set_flag(l, LV_OBJ_FLAG_HIDDEN, text[0] == '\0');
    if (!lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(l, color, 0);
    }
}

static void set_chip(lv_obj_t *row, const char *glyph, lv_color_t tone)
{
    lv_obj_t *chip = ui_find(row, "chip");
    lv_obj_t *icon = ui_find(chip, "icon");
    if (chip == NULL || icon == NULL) {
        return;
    }
    if (!lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), tone) ||
        strcmp(lv_label_get_text(icon), glyph) != 0) {
        ui_chip_tone(chip, glyph, tone);
    }
}

/* ---- §2: the installer menu's row ------------------------------------------------------------ */

static lv_obj_t *s_menu_row; /* while the installer menu shows it */
static uint32_t s_menu_polled;

/* "5 devices", "No devices", "Not connected", "Starting"; "Radio problem" in amber. */
static void menu_words(lv_obj_t *row, const ui_zw_status_t *st)
{
    char value[32];
    lv_color_t color = COLOR_TEXT_2;
    switch (st->radio) {
    case UI_ZW_RADIO_WORKING:
        if (st->jammed) {
            /* TR: the Z-Wave row's value in the installer menu, and after "Radio · " on its
             * page. At most 31 bytes */
            i18n_utf8_copy(value, sizeof(value), TR_C("radio state", "Interference"));
            color = ui_tone_amber();
        } else {
            count_words(st->devices, value, sizeof(value));
        }
        break;
    case UI_ZW_RADIO_NOT_CONNECTED:
    case UI_ZW_RADIO_STARTING:
        i18n_utf8_copy(value, sizeof(value), radio_state_word(st->radio));
        break;
    default:
        /* TR: the Z-Wave row's value in the installer menu. At most 31 bytes */
        i18n_utf8_copy(value, sizeof(value), TR("Radio problem"));
        color = ui_tone_amber();
        break;
    }
    set_words(row, "value_label", value, color);
}

void ui_zwave_menu_row(lv_obj_t *body)
{
    ui_zw_status_t st;
    if (!read_status(&st)) {
        return;
    }
    char words[96];
    if (ui_link_keypad()) {
        /* TR: a keypad's installer menu, the Z-Wave row's detail; {0} is its alarm panel's name
         * (up to 47 bytes). At most 95 bytes */
        I18N_FORMAT(words, sizeof(words), TR("Sensors on {0}'s Z-Wave radio"), ui_link_master_name());
    } else {
        /* TR: a row or paragraph on a Z-Wave page */
        i18n_utf8_copy(words, sizeof(words), TR("Sensors on the panel's Z-Wave radio"));
    }
    s_menu_row = page_row(body, UI_ICON_RADIO_TOWER, ui_tone_blue(),
                          NOTR("Z-Wave") /* Z-Wave term */, words, "-", PAGE_ZWAVE);
    menu_words(s_menu_row, &st);
    s_menu_polled = lv_tick_get();
}

/* ---- §3: the Z-Wave page --------------------------------------------------------------------- */

typedef struct {
    uint16_t node, generation;
    lv_obj_t *row;
} shown_t;

static struct {
    lv_obj_t *radio;      /* the page is on screen while this is set */
    lv_obj_t *list;       /* the devices' rows */
    lv_obj_t *empty;      /* "No devices yet" */
    lv_obj_t *region;
    lv_obj_t *check_in;
    lv_obj_t *smart;      /* "Waiting to join" (SmartStart), when the backend has it */
    lv_obj_t *routes;     /* "Rebuild routes" (installer) */
    lv_obj_t *routes_note;                        /* BL-152: the waiting devices, or the result */
    lv_obj_t *routes_item[UI_ZW_ROUTES_LIST];     /* the devices named, each opens its page */
    size_t mark;          /* the device rows' bindings start here */
    shown_t *shown;       /* UI_ZW_DEVICES_MAX, from ui_slow_alloc */
    size_t nshown;
    ui_zw_device_t *devs; /* UI_ZW_DEVICES_MAX, from ui_slow_alloc */
    uint32_t polled;
} s_net;

typedef struct {
    const char *glyph;
    lv_color_t tone;
    char title[48];
    char detail[96];
    lv_color_t detail_color;
} look_t;

/* "Radio · Working": the Z-Wave radio's row title, its state (translated) after the word. */
static void radio_title(look_t *l, const char *state)
{
    /* TR: the first word of the Z-Wave radio's row title, before " · " and its state. At most
     * 15 bytes */
    snprintf(l->title, sizeof(l->title), "%s · %s", TR_C("Z-Wave radio", "Radio"), state);
}

static void radio_look(const ui_zw_status_t *st, look_t *l)
{
    char devices[24];
    count_words(st->devices, devices, sizeof(devices));
    l->glyph = UI_ICON_RADIO_TOWER;
    l->tone = ui_tone_amber();
    l->detail_color = COLOR_TEXT_2;
    l->detail[0] = '\0';
    switch (st->radio) {
    case UI_ZW_RADIO_WORKING: {
        if (st->jammed) { /* network.md §9 */
            radio_title(l, TR_C("radio state", "Interference"));
            /* TR: under the Z-Wave radio's row on the Z-Wave page */
            i18n_utf8_copy(l->detail, sizeof(l->detail),
                           TR("Something drowns out its signals: sensors may not be heard"));
            l->detail_color = ui_tone_amber();
            break;
        }
        if (st->storage_full) { /* ZW-039: Add, Replace and SmartStart wait for room */
            radio_title(l, radio_state_word(UI_ZW_RADIO_WORKING));
            /* TR: under the Z-Wave radio's row on the Z-Wave page (Zigbee's words). At most 95
             * bytes */
            i18n_utf8_copy(l->detail, sizeof(l->detail),
                           TR("The panel's storage is full: no new device can be added"));
            l->detail_color = ui_tone_amber();
            break;
        }
        /* TR: the state of the Z-Wave radio, a row's value */
        radio_title(l, st->update_advised ? TR_C("radio state", "Update its software")
                                          : radio_state_word(UI_ZW_RADIO_WORKING));
        l->tone = st->update_advised ? ui_tone_amber() : ui_tone_green();
        if (st->region_known) {
            join(l->detail, sizeof(l->detail), region_name(st->region));
        }
        join(l->detail, sizeof(l->detail), devices);
        if (st->devices > 0u) {
            char heard[24];
            if (st->not_heard == 0u) {
                /* TR: under the Z-Wave radio's row: every device answered lately. At most 23
                 * bytes */
                i18n_utf8_copy(heard, sizeof(heard), TR("all heard"));
            } else {
                /* TR: under the Z-Wave radio's row: how many devices are silent. At most 20
                 * bytes */
                I18N_FORMAT(heard, sizeof(heard),
                            TRN("{0} not heard", "{0} not heard", st->not_heard),
                            (unsigned)st->not_heard);
                l->detail_color = ui_tone_amber();
            }
            join(l->detail, sizeof(l->detail), heard);
        }
        break;
    }
    case UI_ZW_RADIO_STARTING:
        radio_title(l, radio_state_word(UI_ZW_RADIO_STARTING));
        /* TR: under the Z-Wave radio's row on the Z-Wave page */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Checking the radio and its devices"));
        l->tone = ui_tone_gray();
        break;
    case UI_ZW_RADIO_NOT_ANSWERING:
        radio_title(l, radio_state_word(UI_ZW_RADIO_NOT_ANSWERING));
        /* TR: under the Z-Wave radio's row on the Z-Wave page */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Check its wires; the panel keeps trying"));
        break;
    case UI_ZW_RADIO_UNUSABLE:
        radio_title(l, radio_state_word(UI_ZW_RADIO_UNUSABLE));
        /* TR: under the Z-Wave radio's row on the Z-Wave page */
        i18n_utf8_copy(l->detail, sizeof(l->detail),
                       TR("It isn't a Z-Wave controller the panel can use"));
        break;
    case UI_ZW_RADIO_FOREIGN:
        radio_title(l, radio_state_word(UI_ZW_RADIO_FOREIGN));
        /* TR: under the Z-Wave radio's row on the Z-Wave page */
        i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Reset it to use it here"));
        break;
    case UI_ZW_RADIO_UPDATING:
        radio_title(l, radio_state_word(UI_ZW_RADIO_UPDATING));
        /* TR: under the Z-Wave radio's row on the Z-Wave page */
        i18n_utf8_copy(l->detail, sizeof(l->detail),
                       TR("Its own software; sensors are heard again after"));
        break;
    case UI_ZW_RADIO_REPLACED:
        radio_title(l, radio_state_word(UI_ZW_RADIO_REPLACED));
        if (st->replaced == 0u) {
            /* TR: under the Z-Wave radio's row on the Z-Wave page */
            i18n_utf8_copy(l->detail, sizeof(l->detail), TR("Add its devices again"));
        } else {
            /* TR: under the Z-Wave radio's row on the Z-Wave page; {0} is a number */
            I18N_FORMAT(l->detail, sizeof(l->detail),
                        TRN("Add its {0} device again", "Add its {0} devices again", st->replaced),
                        (unsigned)st->replaced);
        }
        break;
    default: /* UI_ZW_RADIO_NOT_CONNECTED */
        radio_title(l, radio_state_word(UI_ZW_RADIO_NOT_CONNECTED));
        /* TR: under the Z-Wave radio's row on the Z-Wave page ("Flasher" is a program's name) */
        i18n_utf8_copy(l->detail, sizeof(l->detail),
                       TR("Choose where it is wired: Flasher or console"));
        l->tone = ui_tone_gray();
        break;
    }
}

/* "Getting new software: 46 %" while this device's update runs (the list's row, B015 UX-036). */
static bool updating_words(const ui_zw_device_t *d, char *out, size_t len)
{
    ui_zw_progress_t p;
    read_progress(&p);
    if (p.op != UI_ZW_OP_FIRMWARE || p.step == UI_ZW_STEP_DONE || p.step == UI_ZW_STEP_IDLE ||
        p.node != d->node || p.generation != d->generation) {
        return false;
    }
    if (p.fw_wake) {
        /* TR: under a device's row while its software is sent. At most 39 bytes */
        i18n_utf8_copy(out, len, TR("Getting new software: wake it"));
    } else if (p.fw_total == 0u) {
        /* TR: under a device's row while its software is sent. At most 39 bytes */
        i18n_utf8_copy(out, len, TR("Getting new software"));
    } else {
        /* TR: under a device's row while its software is sent; {0} is a percentage 0 to 100.
         * At most 35 bytes */
        I18N_FORMAT(out, len, TR("Getting new software: {0} %"),
                    (unsigned)((uint32_t)p.fw_done * 100u / p.fw_total));
    }
    return true;
}
/* BL-152: a finished device read again - the panel keeps its setup's steps all done while it
 * reads it (it works on its old facts), so its state's words are a re-read's: reading, waiting
 * for its wake-up, or stopped. Derived from the panel, not from this visit to the page. */
static bool rereading(const ui_zw_device_t *d)
{
    return (d->state == UI_ZW_DEV_SETTING_UP || d->state == UI_ZW_DEV_WAKE_NEEDED ||
            d->state == UI_ZW_DEV_SETUP_STOPPED) &&
           d->setup_total != 0u && d->setup_done >= d->setup_total;
}


static void device_look(const ui_zw_device_t *d, look_t *l, char *value, size_t vlen)
{
    l->glyph = ui_device_class_glyph((alarm_device_class_t)d->device_class);
    l->tone = ui_tone_green();
    l->detail_color = COLOR_TEXT_2;
    device_name(d, l->title, sizeof(l->title));
    zones_words(d, value, vlen);
    l->detail[0] = '\0';
    if (d->zone_count > 0u) { /* named after its zone: the model goes first on its line */
        char model[UI_ZW_MODEL_LEN];
        ui_zwave_short_model(d->model, model, sizeof(model));
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), model[0] != '\0' ? model : TR("Unknown model"));
    }
    bool warn = false;
    char words[40];
    if (d->state == UI_ZW_DEV_NOT_USABLE) {
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), TR("Another controller: remove it"));
        warn = true;
    } else if (d->state == UI_ZW_DEV_NO_REPORTS) {
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), TR("It can't report to the panel"));
        warn = true;
    } else if (rereading(d) && !d->unreachable && !d->battery_low) {
        /* BL-152: read again - it works on as before, nothing to warn of (unless it stopped
         * answering or its battery is low: those words come first, below) */
        join(l->detail, sizeof(l->detail),
             d->state == UI_ZW_DEV_WAKE_NEEDED
                 /* TR: under a device's row in the device list: its setup is read again when it
                  * next wakes up. At most 39 bytes */
                 ? TR("Read again at its next wake-up")
                 : d->state == UI_ZW_DEV_SETUP_STOPPED
                       /* TR: under a device's row in the device list: reading its setup again
                        * stopped; it works as before. At most 39 bytes */
                       ? TR("Read again stopped: works as before")
                       /* TR: under a device's row in the device list. At most 39 bytes */
                       : TR("Being read again"));
    } else if (d->state == UI_ZW_DEV_WAKE_NEEDED && !rereading(d)) {
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), TR("Wake it to finish setting it up"));
        warn = true;
    } else if (d->state == UI_ZW_DEV_JOINING) {
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), TR("Being added: its question waits on Add a device"));
        warn = true;
    } else if (d->state == UI_ZW_DEV_SETUP_STOPPED && !rereading(d)) {
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), TR("Setup stopped: open it to try again"));
        warn = true;
    } else if (d->state == UI_ZW_DEV_SETTING_UP && !rereading(d)) {
        /* TR: under a device's row in the device list */
        join(l->detail, sizeof(l->detail), TR("Being set up"));
    } else if (updating_words(d, words, sizeof(words))) {
        join(l->detail, sizeof(l->detail), words); /* B015 UX-036 */
    } else if (d->unreachable) {
        if (d->heard_s == UI_ZW_NEVER) {
            /* TR: under a device's row in the device list. At most 39 bytes */
            i18n_utf8_copy(words, sizeof(words), TR("Not heard since the panel started"));
        } else {
            char span[24];
            span_words(d->heard_s, span, sizeof(span));
            /* TR: under a device's row in the device list; {0} is a span such as "45 min".
             * At most 21 bytes */
            I18N_FORMAT(words, sizeof(words), TR("Not heard for {0}"), span);
        }
        join(l->detail, sizeof(l->detail), words);
        warn = true;
    } else {
        join(l->detail, sizeof(l->detail), security_short(d->security));
        warn = security_warns(d->security);
        if (d->battery_low) {
            /* TR: under a device's row in the device list */
            join(l->detail, sizeof(l->detail), TR("Battery low"));
            warn = true;
        } else if (d->battery_pct != UI_ZW_BATTERY_UNKNOWN) {
            /* TR: under a device's row in the device list; {0} is a percentage 0 to 100.
             * At most 35 bytes */
            I18N_FORMAT(words, sizeof(words), TR("Battery {0} %"), (unsigned)d->battery_pct);
            join(l->detail, sizeof(l->detail), words);
        }
    }
    if (warn) {
        l->tone = ui_tone_amber();
        l->detail_color = ui_tone_amber();
    }
    if (d->state == UI_ZW_DEV_NOT_USABLE || d->state == UI_ZW_DEV_NO_REPORTS) {
        l->tone = ui_tone_gray();
    }
}

/* Unreachable devices first, then by name (screens.md §3). */
static bool before(const ui_zw_device_t *a, const ui_zw_device_t *b)
{
    if (a->unreachable != b->unreachable) {
        return a->unreachable;
    }
    char na[48];
    char nb[48];
    device_name(a, na, sizeof(na));
    device_name(b, nb, sizeof(nb));
    const int c = strcmp(na, nb);
    return c != 0 ? c < 0 : a->node < b->node;
}

static size_t read_devices(ui_zw_device_t *out)
{
    if (S.be.zw_devices == NULL) {
        return 0u;
    }
    size_t n = S.be.zw_devices(S.be.ctx, out, UI_ZW_DEVICES_MAX);
    n = n < UI_ZW_DEVICES_MAX ? n : UI_ZW_DEVICES_MAX;
    for (size_t i = 1; i < n; i++) { /* a few dozen at most: insertion */
        const ui_zw_device_t d = out[i];
        size_t j = i;
        for (; j > 0u && before(&d, &out[j - 1u]); j--) {
            out[j] = out[j - 1u];
        }
        out[j] = d;
    }
    return n;
}

static void open_device(void *ctx)
{
    if (page_builder(PAGE_ZWAVE_DEVICE) != NULL) {
        ui_page_open(PAGE_ZWAVE_DEVICE, (int32_t)(intptr_t)ctx);
    }
}

/* A device's row: its page (arg = node | generation << 16) once that is built. */
static lv_obj_t *device_row(lv_obj_t *list, const ui_zw_device_t *d)
{
    const uint32_t arg = (uint32_t)d->node | (uint32_t)d->generation << 16;
    const bool there = page_builder(PAGE_ZWAVE_DEVICE) != NULL;
    return ui_row_link(list, UI_ICON_RADIO_TOWER, ui_tone_green(), "-", "-", "-",
                       there ? UI_ICON_CHEVRON_RIGHT : NULL, there ? open_device : NULL,
                       (void *)(intptr_t)(int32_t)arg);
}

static void device_words_to(lv_obj_t *row, const ui_zw_device_t *d)
{
    look_t l;
    char value[32];
    device_look(d, &l, value, sizeof(value));
    set_chip(row, l.glyph, l.tone);
    set_words(row, "title_label", l.title, COLOR_TEXT);
    set_words(row, "detail_label", l.detail, l.detail_color);
    set_words(row, "value_label", value, COLOR_TEXT_2);
}

/* The device rows, made again when the devices or their order changed, else only their words. */
static void show_devices(void)
{
    if (s_net.list == NULL || s_net.devs == NULL || s_net.shown == NULL) {
        return;
    }
    const size_t n = read_devices(s_net.devs);
    bool same = n == s_net.nshown;
    for (size_t i = 0; same && i < n; i++) {
        same = s_net.shown[i].node == s_net.devs[i].node &&
               s_net.shown[i].generation == s_net.devs[i].generation;
    }
    if (!same) {
        lv_obj_clean(s_net.list);
        ui_rows_release(s_net.mark);
        s_net.nshown = 0;
        for (size_t i = 0; i < n; i++) {
            const ui_zw_device_t *d = &s_net.devs[i];
            s_net.shown[s_net.nshown].node = d->node;
            s_net.shown[s_net.nshown].generation = d->generation;
            s_net.shown[s_net.nshown].row = device_row(s_net.list, d);
            s_net.nshown++;
        }
    }
    for (size_t i = 0; i < s_net.nshown; i++) {
        device_words_to(s_net.shown[i].row, &s_net.devs[i]);
    }
    if (s_net.empty != NULL) {
        lv_obj_set_flag(s_net.empty, LV_OBJ_FLAG_HIDDEN, n > 0u);
    }
}

static void smart_row_words(void);
static void routes_row_words(const ui_zw_status_t *st);
static void routes_result_words(void);
static void routes_note_tap(void *ctx);
static void routes_item_tap(void *ctx);

static void show_status(void)
{
    ui_zw_status_t st;
    if (!read_status(&st)) {
        return;
    }
    look_t l;
    radio_look(&st, &l);
    set_chip(s_net.radio, l.glyph, l.tone);
    set_words(s_net.radio, "title_label", l.title, COLOR_TEXT);
    set_words(s_net.radio, "detail_label", l.detail, l.detail_color);
    if (s_net.region != NULL) {
        set_words(s_net.region, "value_label", st.region_known ? region_name(st.region)
                                                : TR_C("Z-Wave region", "Unknown"),
                  COLOR_TEXT_2);
    }
    if (s_net.check_in != NULL) {
        char words[32];
        check_in_words(st.check_in_s, words, sizeof(words));
        set_words(s_net.check_in, "value_label", words, COLOR_TEXT_2);
    }
    smart_row_words();
    routes_row_words(&st);
}

/* ---- §3 "Rebuild routes" (network.md §11) ---------------------------------------------------- */

static void routes_request(uint8_t kind, uint16_t node, uint16_t generation)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    (void)request(&req);
}

static void routes_go(void)
{
    routes_request(UI_ZW_REQ_ROUTES, 0u, 0u);
    show_status();
}

static void routes_stop_go(void)
{
    routes_request(UI_ZW_REQ_STOP, 0u, 0u);
}

static void routes_tap(void *ctx)
{
    (void)ctx;
    ui_zw_status_t st;
    if (read_status(&st) && st.routes_total > 0u) {
        ui_confirm(TR("Stop rebuilding routes?"),
                   TR("The devices not done yet keep the routes they have."),
                   TR_C("confirm button", "Stop"), routes_stop_go);
        return;
    }
    ui_confirm(TR("Rebuild routes?"),
               TR("Each device is asked for its neighbours again, one at a time. A battery "
                  "sensor waits until it wakes: press its button."),
               TR_C("confirm button", "Rebuild"), routes_go);
}

/* "3 of 7 done" while it runs; its end said once, then put away. */
static void routes_row_words(const ui_zw_status_t *st)
{
    if (s_net.routes == NULL) {
        return;
    }
    char value[32] = "";
    if (st->routes_total > 0u) {
        /* TR: the Rebuild routes row's value while it runs, "3 of 7 done". At most 25 bytes */
        I18N_FORMAT(value, sizeof(value), TR("{0} of {1} done"), (unsigned)st->routes_done,
                    (unsigned)st->routes_total);
    }
    set_words(s_net.routes, "value_label", value, COLOR_TEXT_2);
    routes_result_words();
}

/* A device of the rebuild's list, by name (its model or zone), or "Device 7". */
static void routes_name(uint16_t node, char *out, size_t len)
{
    const size_t n = s_net.devs != NULL ? read_devices(s_net.devs) : 0u;
    for (size_t i = 0; i < n; i++) {
        if (s_net.devs[i].node == node) {
            device_name(&s_net.devs[i], out, len);
            return;
        }
    }
    /* TR: a Z-Wave device with no name yet; {0} is its number. At most 20 bytes */
    I18N_FORMAT(out, len, TR("Device {0}"), (unsigned)node);
}

static void routes_item_tap(void *ctx)
{
    ui_zw_progress_t p;
    read_progress(&p);
    const size_t i = (size_t)(intptr_t)ctx;
    if (p.op != UI_ZW_OP_ROUTES || i >= p.routes_list_n) {
        return;
    }
    const size_t n = s_net.devs != NULL ? read_devices(s_net.devs) : 0u;
    for (size_t k = 0; k < n; k++) {
        if (s_net.devs[k].node == p.routes_list[i].node) {
            open_device((void *)(intptr_t)(int32_t)((uint32_t)s_net.devs[k].node |
                                                    (uint32_t)s_net.devs[k].generation << 16));
            return;
        }
    }
}

/* Tapping the result puts it away (a running job's note does nothing). */
static void routes_note_tap(void *ctx)
{
    (void)ctx;
    ui_zw_progress_t p;
    read_progress(&p);
    if (p.op == UI_ZW_OP_ROUTES && p.step == UI_ZW_STEP_DONE && p.generation == 0u) {
        routes_request(UI_ZW_REQ_DISMISS, 0u, 0u);
        show_status();
    }
}

/* BL-152: while it runs, the sleepers to wake; at its end, the result and the devices that need a
 * look, kept until the installer clears it (a single device's job: its page says it). */
static void routes_result_words(void)
{
    if (s_net.routes_note == NULL) {
        return;
    }
    ui_zw_progress_t p;
    read_progress(&p);
    const bool mine = p.op == UI_ZW_OP_ROUTES && p.generation == 0u;
    const bool done = mine && p.step == UI_ZW_STEP_DONE;
    const bool running = mine && !done && p.step != UI_ZW_STEP_IDLE;
    char title[96] = "";
    char detail[96] = "";
    size_t shown = 0u;
    if (running && p.routes_waiting > 0u) {
        char names[64] = "";
        size_t used = 0u;
        for (size_t i = 0; i < p.routes_list_n && p.routes_list[i].what == UI_ZW_ROUTE_WAITING;
             i++) {
            char one[32];
            routes_name(p.routes_list[i].node, one, sizeof(one));
            const int w = snprintf(names + used, sizeof(names) - used, "%s%s",
                                   used > 0u ? ", " : "", one);
            if (w < 0 || (size_t)w >= sizeof(names) - used) {
                names[used] = '\0';
                break;
            }
            used += (size_t)w;
        }
        /* TR: while rebuilding routes: sleeping devices to wake up; {0} is their names. At most 60 bytes */
        I18N_FORMAT(title, sizeof(title), TR("Wake these: {0}"), names);
        /* TR: a row or paragraph on a Z-Wave page */
        i18n_utf8_copy(detail, sizeof(detail), TR("Press each one's button"));
    } else if (done) {
        const unsigned good = (unsigned)(p.routes_done - p.routes_failed - p.routes_skipped);
        if (p.routes_stopped) {
            /* TR: the result when rebuilding routes was stopped; {0} of {1} devices */
            I18N_FORMAT(title, sizeof(title),
                        TRN("Stopped: {0} of {1} device done", "Stopped: {0} of {1} devices done",
                            p.routes_total),
                        good, (unsigned)p.routes_total);
        } else if (good == p.routes_total) {
            /* TR: the result when every device's routes were rebuilt; {0} is the number of devices */
            I18N_FORMAT(title, sizeof(title),
                        TRN("Routes rebuilt for {0} device", "Routes rebuilt for all {0} devices",
                            good),
                        good);
        } else {
            /* TR: the result when rebuilding routes ended; {0} of {1} devices were done */
            I18N_FORMAT(title, sizeof(title), TR("Routes rebuilt for {0} of {1}"), good,
                        (unsigned)p.routes_total);
        }
        if (p.routes_failed > 0u && p.routes_skipped > 0u) {
            /* TR: under the rebuild result: {0} devices didn't answer, {1} were skipped */
            I18N_FORMAT(detail, sizeof(detail), TR("{0} didn't answer · {1} skipped"),
                        (unsigned)p.routes_failed, (unsigned)p.routes_skipped);
        } else if (p.routes_failed > 0u) {
            /* TR: under the rebuild result: {0} devices didn't answer */
            I18N_FORMAT(detail, sizeof(detail), TR("{0} didn't answer"), (unsigned)p.routes_failed);
        } else if (p.routes_skipped > 0u) {
            /* TR: under the rebuild result: {0} devices were skipped (Long Range, or dead) */
            I18N_FORMAT(detail, sizeof(detail), TR("{0} skipped"), (unsigned)p.routes_skipped);
        } else {
            /* TR: under the rebuild result, to put it away */
            i18n_utf8_copy(detail, sizeof(detail), TR("Tap to clear"));
        }
        shown = p.routes_list_n;
    }
    lv_obj_set_flag(s_net.routes_note, LV_OBJ_FLAG_HIDDEN, title[0] == '\0');
    set_words(s_net.routes_note, "title_label", title, COLOR_TEXT);
    set_words(s_net.routes_note, "detail_label", detail, COLOR_TEXT_2);
    set_words(s_net.routes_note, "value_label", done ? TR_C("button", "Clear") : "", COLOR_TEXT_2);
    for (size_t i = 0; i < UI_ZW_ROUTES_LIST; i++) {
        lv_obj_t *row = s_net.routes_item[i];
        if (row == NULL) {
            continue;
        }
        lv_obj_set_flag(row, LV_OBJ_FLAG_HIDDEN, i >= shown);
        if (i >= shown) {
            continue;
        }
        char name[48];
        routes_name(p.routes_list[i].node, name, sizeof(name));
        set_words(row, "title_label", name, COLOR_TEXT);
        const char *why;
        switch (p.routes_list[i].what) {
        case UI_ZW_ROUTE_WAITING: /* TR: why a device is listed after rebuilding routes. At most 40 bytes */
            why = TR_C("routes result", "Still waiting for its wake-up"); break;
        case UI_ZW_ROUTE_FAILED:
            why = TR_C("routes result", "Didn't answer"); break;
        case UI_ZW_ROUTE_SKIPPED:
            why = TR_C("routes result", "Skipped: dead, Long Range or stopped"); break;
        default:
            why = TR_C("routes result", "Its way to the panel wasn't renewed"); break;
        }
        set_words(row, "detail_label", why, COLOR_TEXT_2);
    }
}

/* The check-in battery devices get when they are added (1 h, 4 h or 12 h; network.md §7). */
static const uint32_t CHECK_IN_S[] = {3600u, 14400u, 43200u};

static void check_in_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= sizeof(CHECK_IN_S) / sizeof(CHECK_IN_S[0]) || !installer()) {
        return;
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_CHECK_IN;
    req.user = S.auth_user;
    req.seconds = CHECK_IN_S[index];
    if (request(&req)) {
        show_status();
    }
}

static void pick_check_in(void *ctx)
{
    (void)ctx;
    ui_zw_status_t st;
    if (!read_status(&st)) {
        return;
    }
    static ui_choice_t choices[sizeof(CHECK_IN_S) / sizeof(CHECK_IN_S[0])];
    static char words[sizeof(CHECK_IN_S) / sizeof(CHECK_IN_S[0])][24];
    int32_t selected = -1;
    for (size_t i = 0; i < sizeof(CHECK_IN_S) / sizeof(CHECK_IN_S[0]); i++) {
        check_in_words(CHECK_IN_S[i], words[i], sizeof(words[i]));
        choices[i].glyph = NULL;
        choices[i].tone = ui_tone_blue();
        choices[i].title = words[i];
        /* TR: a choice's note in the check-in list */
        choices[i].detail = i == 0u   ? TR("A silent sensor shows soonest")
                            /* TR: a choice's note in the check-in list */
                            : i == 2u ? TR("The longest battery life")
                                      : NULL;
        if (CHECK_IN_S[i] == st.check_in_s) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Check-in for new devices"),
                  TR("How often a battery sensor added from now on reports in. Each device's own "
                     "is on its page."),
                  choices, sizeof(CHECK_IN_S) / sizeof(CHECK_IN_S[0]), selected, check_in_chosen);
}

static lv_obj_t *device_list(lv_obj_t *body)
{
    lv_obj_t *list = lv_obj_create(body);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, SPACE_SM, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_CLICKABLE); /* a press between rows is the page's */
    return list;
}

static void build_network(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                          size_t nlen)
{
    (void)arg;
    i18n_utf8_copy(title, tlen, NOTR("Z-Wave")); /* Z-Wave term */
    /* TR: the Z-Wave page's note line under its title */
    i18n_utf8_copy(note, nlen, TR("Sensors paired to the panel's Z-Wave radio"));
    if (s_net.shown == NULL) {
        s_net.shown = ui_slow_alloc(UI_ZW_DEVICES_MAX * sizeof(*s_net.shown));
    }
    if (s_net.devs == NULL) {
        s_net.devs = ui_slow_alloc(UI_ZW_DEVICES_MAX * sizeof(*s_net.devs));
    }
    ui_zw_status_t st;
    if (!read_status(&st) || s_net.shown == NULL || s_net.devs == NULL) {
        ui_paragraph(body, TR("Z-Wave isn't available on this panel."), ui_tone_gray());
        return;
    }
    s_net.radio = page_row(body, UI_ICON_RADIO_TOWER, ui_tone_gray(), "-", "-", NULL,
                           PAGE_ZWAVE_RADIO);
    const bool can = installer();
    if (can) {
        ui_zw_progress_t pr;
        read_progress(&pr);
        const bool waits = pr.op == UI_ZW_OP_ADD &&
                           (pr.step == UI_ZW_STEP_ASK_ALLOW || pr.step == UI_ZW_STEP_ASK_PIN);
        /* TR: a row or paragraph on a Z-Wave page */
        lv_obj_t *add = page_row(body, UI_ICON_PLUS, ui_tone_blue(), TR("Add a device"),
                                 /* TR: a row or paragraph on a Z-Wave page */
                                 waits ? TR("A device waits for your answer")
                                       /* TR: a row or paragraph on a Z-Wave page */
                                       : TR("Pair a sensor with the panel"),
                                 NULL, PAGE_ZWAVE_ADD);
        if (waits) {
            set_words(add, "detail_label", TR("A device waits for your answer"), ui_tone_amber());
        }
        /* TR: a row or paragraph on a Z-Wave page */
        page_row(body, UI_ICON_TRASH, ui_tone_gray(), TR("Remove a device"),
                 /* TR: a row or paragraph on a Z-Wave page */
                 TR("Also resets a sensor from another system"), NULL, PAGE_ZWAVE_REMOVE);
    }
    /* TR: a section heading on the Z-Wave page */
    ui_section(body, TR_C("Z-Wave section", "Devices"));
    s_net.list = device_list(body);
    s_net.empty = ui_paragraph(
        /* TR: a row or paragraph on a Z-Wave page */
        body, TR("No devices yet. Add a device to pair a sensor with the panel."), ui_tone_gray());
    /* TR: a section heading on the Z-Wave page */
    ui_section(body, TR_C("Z-Wave section", "Network"));
    /* TR: a row's title on the Z-Wave page */
    s_net.region = page_row(body, UI_ICON_GLOBE, ui_tone_blue(), TR_C("Z-Wave settings", "Region"),
                            /* TR: a row or paragraph on a Z-Wave page */
                            TR("Match your radio"), "-", PAGE_ZWAVE_REGION);
    s_net.check_in = ui_row_link(body, UI_ICON_CLOCK, ui_tone_blue(),
                                 TR("Check-in for new devices"),
                                 /* TR: a row or paragraph on a Z-Wave page */
                                 TR("How often battery sensors report in"), "-",
                                 can ? UI_ICON_CHEVRON_RIGHT : NULL, can ? pick_check_in : NULL,
                                 NULL);
    s_net.smart = S.be.zw_prov != NULL
                      /* TR: a row or paragraph on a Z-Wave page */
                      ? page_row(body, UI_ICON_PLUG, ui_tone_blue(), TR("Waiting to join"),
                                 /* TR: a row or paragraph on a Z-Wave page */
                                 TR("Sensors registered by their QR code"), "-",
                                 PAGE_ZWAVE_SMART)
                      : NULL;
    /* TR: a row or paragraph on a Z-Wave page */
    s_net.routes = can ? ui_row_link(body, UI_ICON_WAVES, ui_tone_blue(), TR("Rebuild routes"),
                                     /* TR: a row or paragraph on a Z-Wave page */
                                     TR("Refresh how signals reach the panel"), "-",
                                     UI_ICON_CHEVRON_RIGHT, routes_tap, NULL)
                       : NULL;
    s_net.routes_note = NULL;
    memset(s_net.routes_item, 0, sizeof(s_net.routes_item));
    if (can) {
        s_net.routes_note = ui_row_link(body, UI_ICON_WAVES, ui_tone_blue(), "-", "-", "-", NULL,
                                        routes_note_tap, NULL);
        lv_obj_add_flag(s_net.routes_note, LV_OBJ_FLAG_HIDDEN);
        for (size_t i = 0; i < UI_ZW_ROUTES_LIST; i++) {
            s_net.routes_item[i] = ui_row_link(body, UI_ICON_RADIO_TOWER, ui_tone_amber(), "-",
                                               "-", NULL, UI_ICON_CHEVRON_RIGHT, routes_item_tap,
                                               (void *)(intptr_t)i);
            lv_obj_add_flag(s_net.routes_item[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_t *reset = ui_row_link(body, UI_ICON_TRIANGLE_ALERT, ui_tone_red(),
                                      /* TR: a row or paragraph on a Z-Wave page */
                                      TR("Reset the Z-Wave network"),
                                      TR("Every device must be added again"), NULL,
                                      UI_ICON_CHEVRON_RIGHT, reset_row_tap, NULL);
        set_words(reset, "title_label", TR("Reset the Z-Wave network"), ui_tone_red());
    } else {
        /* TR: a row or paragraph on a Z-Wave page */
        ui_paragraph(body, TR("Adding and removing devices is done in installer mode."),
                     ui_tone_gray());
    }
    /* The device rows last: their bindings can then be dropped and made again on their own. */
    s_net.mark = ui_rows_mark();
    s_net.nshown = 0;
    show_status();
    show_devices();
    s_net.polled = lv_tick_get();
}

/* ---- §6: the device page (PAGE_ZWAVE_DEVICE, arg = node | generation << 16) ------------------- */

static struct {
    uint16_t node, generation;
    lv_obj_t *signal;     /* the "Signal" row under it (ZW-040), NULL until a frame was measured */
    lv_obj_t *last_heard; /* the "Last heard" row: reread and rewritten in place each second */
    lv_obj_t *check_in;   /* a sleeping device's "Checks in" row: its words the same way */
    lv_obj_t *setup;      /* "Setting it up: step 3 of 16": its words the same way */
    bool open;
    bool unreachable; /* shown structurally (a banner, extra rows): a rebuild when this flips */
    uint8_t state;    /* ui_zw_dev_state_t, same reason (the "still interviewing" banner) */
    uint8_t security; /* ui_zw_security_t, same reason */
    uint8_t fb_fit, fb_partition; /* its sounds and lights: fit and partition, same reason */
    bool fb_live;
    uint8_t kc_fit, kc_partition, kc_panic; /* its buttons: the same */
    bool kc_live;
    bool cleaning;    /* a "reset, broken or gone" removal of it runs: shown, a rebuild when it ends */
    uint8_t fw_check; /* its software check (ui_zw_fw_check_t): a rebuild when it changes */
    uint8_t fw_major, fw_minor; /* its version: a rebuild when it is read again (UX-031) */
    uint32_t outputs_crc; /* its outputs as shown (levels, busy, results): a rebuild when changed */
    bool fw_running;  /* its software update runs: shown, a rebuild when it ends */
    lv_obj_t *fw_line; /* "Sending its new software: 40 %": its words rewritten in place */
    uint32_t polled;
} s_dev;

/* How its software update ended: said on its page until the installer leaves it. */
static struct {
    uint16_t node, generation;
    uint8_t result; /* UI_ZW_RES_FW_UPDATED / _FAILED; UI_ZW_RES_NONE: nothing to say */
    uint8_t why;    /* ui_zw_fw_failure_t */
    uint8_t major, minor; /* its version before (UX-032: "it runs 1.20 now" once read again) */
} s_fwres;

/* How a "reset, broken or gone" removal of the device on its page ended: said on the page until
 * the installer leaves it (a toast would be gone before it is read). */
static struct {
    uint16_t node, generation;
    uint8_t result; /* ui_zw_result_t; UI_ZW_RES_NONE: nothing to say */
} s_clean;

/* ---- §6: a device's own check-in (network.md §7) -------------------------------------------- */

#define CHECK_IN_CHOICES (sizeof(CHECK_IN_S) / sizeof(CHECK_IN_S[0]))

/* The check-ins its page offers - every hour, 4 hours, 12 hours - those inside its own limits when
 * they are known ("Never" is shown, not offered). How many there are. */
static size_t check_in_choices(const ui_zw_device_page_t *page, uint32_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < CHECK_IN_CHOICES; i++) {
        const uint32_t s = CHECK_IN_S[i];
        if ((page->check_in_min_s != 0u && s < page->check_in_min_s) ||
            (page->check_in_max_s != 0u && s > page->check_in_max_s)) {
            continue;
        }
        out[n++] = s;
    }
    return n;
}

/* "Changes to every 12 hours when it next wakes up" while a chosen check-in waits for the device;
 * "" when nothing does. */
static void check_in_next_words(const ui_zw_device_page_t *page, char *out, size_t len)
{
    out[0] = '\0';
    if (page->check_in_next_s != 0u) {
        const uint32_t s = page->check_in_next_s;
        if (s == 3600u) {
            /* TR: under a sleeping device's "Checks in" row. At most 79 bytes */
            i18n_utf8_copy(out, len, TR("Changes to every hour when it next wakes up"));
        } else if (s % 3600u == 0u) {
            /* TR: under a sleeping device's "Checks in" row; {0} is 2 to 12. At most 60 bytes */
            I18N_FORMAT(out, len,
                        TRN("Changes to every {0} hours when it next wakes up",
                            "Changes to every {0} hours when it next wakes up", s / 3600u),
                        (unsigned)(s / 3600u));
        } else {
            /* TR: under a sleeping device's "Checks in" row; {0} is 1 to 59. At most 60 bytes */
            I18N_FORMAT(out, len,
                        TRN("Changes to every {0} min when it next wakes up",
                            "Changes to every {0} min when it next wakes up", (s + 59u) / 60u),
                        (unsigned)((s + 59u) / 60u));
        }
    }
}

/* The "Checks in" row's words, in place: what the device does now and what is to come. */
static void device_check_in_words(const ui_zw_device_page_t *page)
{
    if (s_dev.check_in == NULL) {
        return;
    }
    char now[32];
    char next[80];
    check_in_words(page->check_in_s, now, sizeof(now));
    check_in_next_words(page, next, sizeof(next));
    set_words(s_dev.check_in, "value_label", now, COLOR_TEXT_2);
    set_words(s_dev.check_in, "detail_label", next, COLOR_TEXT_2);
}

static uint32_t s_dev_choice_s[CHECK_IN_CHOICES]; /* the sheet's choices, in seconds */
static size_t s_dev_choice_n;

static void device_check_in_chosen(int32_t index)
{
    ui_zw_device_page_t page;
    if (index < 0 || (size_t)index >= s_dev_choice_n || !installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    const uint32_t seconds = s_dev_choice_s[index];
    if (seconds == (page.check_in_next_s != 0u ? page.check_in_next_s : page.check_in_s)) {
        return; /* what it has, or is about to get */
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_CHECK_IN;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    req.seconds = seconds;
    if (request(&req) && S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        device_check_in_words(&page); /* "Changes to ... when it next wakes up", at once */
        s_dev.polled = lv_tick_get(); /* and not again within the second */
    }
}

/* "Checks in" on a sleeping device's page (screens.md §6): the choices within its limits; it
 * takes the new one when it next wakes up. */
static void pick_device_check_in(void *ctx)
{
    (void)ctx;
    ui_zw_device_page_t page;
    if (!installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    static ui_choice_t choices[CHECK_IN_CHOICES];
    static char words[CHECK_IN_CHOICES][24];
    s_dev_choice_n = check_in_choices(&page, s_dev_choice_s);
    const uint32_t current = page.check_in_next_s != 0u ? page.check_in_next_s : page.check_in_s;
    int32_t selected = -1;
    for (size_t i = 0; i < s_dev_choice_n; i++) {
        const uint32_t s = s_dev_choice_s[i];
        check_in_words(s, words[i], sizeof(words[i]));
        choices[i].glyph = NULL;
        choices[i].tone = ui_tone_blue();
        choices[i].title = words[i];
        choices[i].detail = NULL;
        if (s == CHECK_IN_S[0]) {
            choices[i].detail = TR("A silent sensor shows soonest");
        } else if (s == CHECK_IN_S[CHECK_IN_CHOICES - 1u]) {
            choices[i].detail = TR("The longest battery life");
        }
        if (s == current) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Checks in"), TR("How often it reports in. It changes when it next wakes up."),
                  choices, s_dev_choice_n, selected, device_check_in_chosen);
}

static void open_zone_row(void *ctx)
{
    ui_page_open(PAGE_ZONE, (int32_t)(intptr_t)ctx);
}

static void open_remove_for(void *ctx)
{
    ui_page_open(PAGE_ZWAVE_REMOVE, (int32_t)(intptr_t)ctx);
}

static void ping_go(void *ctx)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_PING;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    if (request(&req)) {
        ui_toast(TR("Checking..."));
    }
}

static void replace_go(void)
{
    s_replace_arg = (int32_t)(intptr_t)s_confirm_arg;
    ui_page_open(PAGE_ZWAVE_ADD, 2);
}

static void replace_ask(void *ctx)
{
    s_confirm_arg = ctx;
    ui_confirm(TR("Replace it with a new device?"),
               TR("Have the new device ready. It takes this one's place, and its zones follow it "
                  "(the same kind of sensor keeps working)."),
               TR("Replace it"), replace_go);
}

static void routes_one_go(void *ctx)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    ui_zw_device_page_t page;
    const bool sleeps = S.be.zw_device != NULL &&
                        S.be.zw_device(S.be.ctx, node, generation, &page) && !page.dev.listening;
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_ROUTES;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    if (request(&req) && sleeps) {
        ui_toast(TR("Wake it now: press its button")); /* the next step, not seen otherwise */
    }
}

static void identify_go(void *ctx)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_IDENTIFY;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    (void)request(&req); /* its answer: device_tick's toast */
}

/* The device whose setup the installer tried again on this visit to its page: its words say so
 * ("Tried again: ...") until the page is left. */
static struct {
    uint16_t node, generation;
} s_retry;

/* Continue a device's setup from the step it stopped at (UI_ZW_REQ_SETUP). */
static void setup_again(void *ctx)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    s_retry.node = node;
    s_retry.generation = generation;
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_SETUP;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    if (request(&req)) {
        ui_page_rebuild(); /* "Setting it up" at once */
    }
}

/* "Set it up again", confirmed: a finished device's setup read again from the start (the same
 * UI_ZW_REQ_SETUP; the panel tells the two apart). */
static void reread_go(void)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)s_confirm_arg, &node, &generation);
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_SETUP;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    if (S.be.zw_request == NULL) {
        return;
    }
    const ui_zw_req_result_t r = S.be.zw_request(S.be.ctx, &req);
    if (r == UI_ZW_REQ_BUSY) {
        /* TR: a toast: Set it up again was refused because the panel reads or does something
         * else (maybe another device's setup). At most 80 bytes */
        ui_toast(TR("The panel is busy with another device: try again when it's done"));
    } else if (r != UI_ZW_REQ_OK) {
        ui_toast(request_refused(r));
    } else {
        ui_page_rebuild(); /* "Reading it again" at once */
    }
}

static void reread_ask(void *ctx)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    ui_zw_device_page_t page;
    const bool sleeps = S.be.zw_device != NULL &&
                        S.be.zw_device(S.be.ctx, node, generation, &page) && !page.dev.listening;
    const char *text;
    if (sleeps) {
        /* TR: a confirmation's text: a battery device's setup read again. At most 120 bytes
         * (the confirmation holds 127) */
        text = TR("It's read again at its next wake-up. Its zones keep working, and it keeps "
                  "its security. Wake it to start now.");
    } else {
        /* TR: a confirmation's text: a device's setup read again. At most 120 bytes (the
         * confirmation holds 127) */
        text = TR("The panel reads what it can do again. Its zones keep working, and it keeps "
                  "its security.");
    }
    s_confirm_arg = ctx;
    /* TR: a confirmation's title. At most 40 bytes */
    ui_confirm(TR("Set it up again?"), text, TR_C("confirm button", "Set it up again"),
               reread_go);
}

/* A keypad's sounds and lights (ui_zw_device_page_t.feedback): Off or one partition. */
static uint8_t s_fb_choice[1u + ALARM_MAX_PARTITIONS]; /* partition per choice, 0: off */
static size_t s_fb_n;

static void feedback_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_fb_n || !installer() || !s_dev.open) {
        return;
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_FEEDBACK;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    req.partition = s_fb_choice[index];
    if (request(&req)) {
        ui_page_rebuild();
    }
}

static void pick_feedback(void *ctx)
{
    (void)ctx;
    ui_zw_device_page_t page;
    if (!installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    static ui_choice_t choices[1u + ALARM_MAX_PARTITIONS];
    static char words[ALARM_MAX_PARTITIONS][16];
    s_fb_n = 0;
    int32_t selected = page.dev.node != 0u && page.feedback.partition == 0u ? 0 : -1;
    choices[0].glyph = NULL;
    choices[0].tone = ui_tone_gray();
    /* TR: a choice's title (sounds and lights of a keypad) */
    choices[0].title = TR_C("keypad feedback", "Off");
    /* TR: a row or paragraph on a Z-Wave page */
    choices[0].detail = TR("It shows nothing from the panel");
    s_fb_choice[s_fb_n++] = 0u;
    const uint8_t parts = S.snap.partitions != 0u ? S.snap.partitions : 1u;
    size_t enabled = 0;
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        enabled += (parts & (1u << i)) != 0u ? 1u : 0u;
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((parts & (1u << i)) == 0u) {
            continue;
        }
        if (enabled == 1u) {
            /* TR: a choice's title (sounds and lights of a keypad) */
            i18n_utf8_copy(words[i], sizeof(words[i]), TR_C("keypad feedback", "On"));
        } else {
            /* TR: a choice's title; {0} is a number. At most 14 bytes */
            I18N_FORMAT(words[i], sizeof(words[i]), TR("Partition {0}"), (unsigned)(i + 1u));
        }
        choices[s_fb_n].glyph = NULL;
        choices[s_fb_n].tone = ui_tone_blue();
        choices[s_fb_n].title = words[i];
        /* TR: a row or paragraph on a Z-Wave page */
        choices[s_fb_n].detail = TR("Arming, delays and alarms");
        if (page.feedback.partition == i + 1u) {
            selected = (int32_t)s_fb_n;
        }
        s_fb_choice[s_fb_n++] = (uint8_t)(i + 1u);
    }
    ui_pick_sheet(TR("Sounds and lights"),
                  TR("What the keypad shows. Its buttons don't control the panel."),
                  choices, s_fb_n, selected, feedback_chosen);
}

/* The keypad's section: panel -> keypad feedback only, and why it can't be set when it can't. */
/* "Off", "On" (one partition) or "Partition 2". */
static void partition_words(uint8_t partition, char *out, size_t len)
{
    if (partition == 0u) {
        i18n_utf8_copy(out, len, TR_C("keypad feedback", "Off"));
    } else if ((S.snap.partitions & (uint8_t)~1u) == 0u) {
        i18n_utf8_copy(out, len, TR_C("keypad feedback", "On"));
    } else {
        /* TR: a row's value on a keypad's page; {0} is a number. At most 14 bytes */
        I18N_FORMAT(out, len, TR("Partition {0}"), (unsigned)partition);
    }
}

/* Why a keypad setting can't be changed now, or NULL. */
static const char *fit_words(uint8_t fit)
{
    switch (fit) {
    /* TR: under a keypad's setting on its page, why it can't be changed now */
    case UI_ZW_FB_NOT_READY: return TR("Finish its setup first");
    /* TR: a row or paragraph on a Z-Wave page */
    case UI_ZW_FB_NOT_SECURE: return TR("Needs secure pairing with its PIN: add it again");
    /* TR: a row or paragraph on a Z-Wave page */
    case UI_ZW_FB_UNSUPPORTED: return TR("Its setup found no way to drive them");
    default: return NULL;
    }
}

/* ---- a keypad's buttons (device to panel, ZW-031) ---- */

static uint8_t s_kc_choice[1u + ALARM_MAX_PARTITIONS];
static size_t s_kc_n;
static uint8_t s_kc_partition; /* the choice waiting for its confirmation */

static void control_request(uint8_t partition, uint8_t panic_mask)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_CONTROL;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    req.partition = partition;
    req.panic_mask = panic_mask;
    if (request(&req)) {
        ui_page_rebuild();
    }
}

static void control_confirmed(void)
{
    ui_zw_device_page_t page;
    if (!s_dev.open || !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    control_request(s_kc_partition, page.control.panic_mask);
}

static void control_chosen(int32_t index)
{
    ui_zw_device_page_t page;
    if (index < 0 || (size_t)index >= s_kc_n || !installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    s_kc_partition = s_kc_choice[index];
    if (s_kc_partition == page.control.partition) {
        return;
    }
    if (s_kc_partition == 0u) {
        control_request(0u, 0u); /* taking it away needs no question */
        return;
    }
    ui_confirm(TR("Let its buttons arm and disarm?"),
               TR("With a user's code only: the panel checks the code, what that user may do, "
                  "and locks out wrong guesses."),
               TR("Allow"), control_confirmed);
}

static void pick_control(void *ctx)
{
    (void)ctx;
    ui_zw_device_page_t page;
    if (!installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    static ui_choice_t choices[1u + ALARM_MAX_PARTITIONS];
    static char words[ALARM_MAX_PARTITIONS][16];
    s_kc_n = 0;
    int32_t selected = page.control.partition == 0u ? 0 : -1;
    choices[0] = (ui_choice_t){NULL, ui_tone_gray(), TR_C("keypad option", "Off"),
                               TR("Its buttons don't control the panel")};
    s_kc_choice[s_kc_n++] = 0u;
    const uint8_t parts = S.snap.partitions != 0u ? S.snap.partitions : 1u;
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        if ((parts & (1u << i)) == 0u) {
            continue;
        }
        partition_words((uint8_t)(i + 1u), words[i], sizeof(words[i]));
        choices[s_kc_n] = (ui_choice_t){NULL, ui_tone_blue(), words[i],
                                        TR("Away, Home and Disarm with a user's code")};
        if (page.control.partition == i + 1u) {
            selected = (int32_t)s_kc_n;
        }
        s_kc_choice[s_kc_n++] = (uint8_t)(i + 1u);
    }
    ui_pick_sheet(TR("Arm and disarm from its buttons"),
                  TR("Codes are the panel's own users' codes."), choices, s_kc_n, selected,
                  control_chosen);
}

#define PANIC_ALL ((uint8_t)((1u << ALARM_PANIC_FIRE) | (1u << ALARM_PANIC_POLICE) | \
                             (1u << ALARM_PANIC_MEDICAL)))

static void panic_chosen(int32_t index)
{
    ui_zw_device_page_t page;
    if (index < 0 || index > 1 || !installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page) ||
        page.control.partition == 0u) {
        return;
    }
    const uint8_t mask = index == 1 ? PANIC_ALL : 0u;
    if (mask != page.control.panic_mask) {
        control_request(page.control.partition, mask);
    }
}

static void pick_panic(void *ctx)
{
    (void)ctx;
    ui_zw_device_page_t page;
    if (!installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    static ui_choice_t choices[2];
    choices[0] = (ui_choice_t){NULL, ui_tone_gray(), TR_C("keypad option", "Off"),
                               TR("Its emergency buttons do nothing")};
    choices[1] = (ui_choice_t){NULL, ui_tone_red(), TR_C("keypad option", "On"),
                               TR("Police, fire and medical raise the alarm: no code needed")};
    ui_pick_sheet(TR("Emergency buttons"), TR("Held as the keypad's manual says."), choices, 2u,
                  page.control.panic_mask != 0u ? 1 : 0, panic_chosen);
}

static void device_feedback_rows(lv_obj_t *body, const ui_zw_device_page_t *page)
{
    ui_section(body, TR("Keypad"));
    char value[24];
    if (page->feedback.fit != UI_ZW_FB_NONE) {
        partition_words(page->feedback.partition, value, sizeof(value));
        const char *detail = fit_words(page->feedback.fit);
        if (detail == NULL && page->feedback.partition != 0u && !page->feedback.live) {
            detail = TR("Waiting for it");
        }
        const bool can = page->feedback.fit == UI_ZW_FB_AVAILABLE;
        ui_row_link(body, UI_ICON_BELL, can ? ui_tone_blue() : ui_tone_gray(),
                    TR("Sounds and lights"),
                    detail, value, can ? UI_ICON_CHEVRON_RIGHT : NULL, can ? pick_feedback : NULL,
                    NULL);
    }
    const bool buttons = page->control.fit == UI_ZW_FB_AVAILABLE;
    if (page->control.fit != UI_ZW_FB_NONE) {
        partition_words(page->control.partition, value, sizeof(value));
        const char *detail = fit_words(page->control.fit);
        if (detail == NULL && page->control.partition != 0u && !page->control.live) {
            detail = TR("Not active: add it and assign it again");
        }
        ui_row_link(body, UI_ICON_LOCK_KEYHOLE, buttons ? ui_tone_blue() : ui_tone_gray(),
                    TR("Arm and disarm from its buttons"), detail, value,
                    buttons ? UI_ICON_CHEVRON_RIGHT : NULL, buttons ? pick_control : NULL, NULL);
        if (page->control.partition != 0u) {
            ui_row_link(body, UI_ICON_SIREN, buttons ? ui_tone_red() : ui_tone_gray(),
                        TR("Emergency buttons"), NULL,
                        page->control.panic_mask != 0u ? TR_C("keypad option", "On")
                                                       : TR_C("keypad option", "Off"),
                        buttons ? UI_ICON_CHEVRON_RIGHT : NULL, buttons ? pick_panic : NULL, NULL);
        }
    }
    ui_paragraph(body,
                 page->control.partition != 0u
                     ? TR("Its own sounds and lights show the panel's state. Its buttons arm "
                          "and disarm with a user's code.")
                     : TR("Its own sounds and lights show the panel's state. Its buttons don't "
                          "arm or disarm the panel."),
                 ui_tone_gray());
}

/* ---- a siren the panel sounds with the alarm (OUT-016..OUT-019; the BL-037 proposal) ---- */

typedef enum {
    ZS_PICK_PARTS = 0,
    ZS_PICK_ALSO,
    ZS_PICK_VOLUME,
    ZS_PICK_SUPERVISION,
} zs_pick_t;

static struct {
    uint8_t what;  /* zs_pick_t */
    size_t n;
    ui_choice_t choices[12];
    uint16_t value[12];
    char words[12][48];
} s_zs;

static uint8_t zs_parts_enabled(void)
{
    return S.snap.partitions != 0u ? S.snap.partitions : 1u;
}

/* "All", "Partition 2", "None" (the Z-Wave pages name partitions by number, as its keypads do). */
static void zs_parts_words(uint8_t parts, bool all_word, char *out, size_t len)
{
    const uint8_t all = zs_parts_enabled();
    parts &= all;
    out[0] = '\0';
    if (parts == 0u) {
        i18n_utf8_copy(out, len, TR_C("partitions", "None"));
        return;
    }
    if (all_word && parts == all) {
        i18n_utf8_copy(out, len, TR_C("partitions", "All"));
        return;
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if ((parts & (1u << i)) == 0u) {
            continue;
        }
        char one[24];
        I18N_FORMAT(one, sizeof(one), TR("Partition {0}"), (unsigned)(i + 1u));
        if (out[0] == '\0') {
            i18n_utf8_copy(out, len, one);
        } else {
            char joined[96];
            I18N_FORMAT(joined, sizeof(joined), TR_C("list of names", "{0}, {1}"), out, one);
            i18n_utf8_copy(out, len, joined);
        }
    }
}

static void zs_add(const char *glyph, lv_color_t tone, const char *title, const char *detail,
                   uint16_t value)
{
    if (s_zs.n >= sizeof(s_zs.value) / sizeof(s_zs.value[0])) {
        return;
    }
    i18n_utf8_copy(s_zs.words[s_zs.n], sizeof(s_zs.words[0]), title);
    s_zs.choices[s_zs.n] = (ui_choice_t){glyph, tone, s_zs.words[s_zs.n], detail};
    s_zs.value[s_zs.n] = value;
    s_zs.n++;
}

static int32_t zs_index(uint16_t value)
{
    for (size_t i = 0; i < s_zs.n; i++) {
        if (s_zs.value[i] == value) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Its settings as the page shows them, changed, sent. */
static void zs_send(const ui_zw_device_page_t *page, uint8_t parts, uint8_t also, bool burglary,
                    bool fire, bool squawk, uint8_t volume, uint16_t minutes)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_SIREN;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    req.endpoint = page->siren.endpoint;
    req.parts = (burglary || fire) ? parts : 0u;
    req.also = (uint8_t)(also & (uint8_t)~parts);
    req.siren_flags = (uint8_t)((burglary ? UI_ZW_SIREN_BURGLARY : 0u) |
                                (fire ? UI_ZW_SIREN_FIRE : 0u) | (squawk ? UI_ZW_SIREN_SQUAWK : 0u));
    req.volume = volume;
    req.minutes = minutes;
    if (request(&req)) {
        ui_page_rebuild();
    }
}

static bool zs_page(ui_zw_device_page_t *page)
{
    return installer() && s_dev.open &&
           S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, page) &&
           page->siren.fit == UI_ZW_FB_AVAILABLE;
}

typedef enum { ZS_BURGLARY = 0, ZS_FIRE, ZS_SQUAWK } zs_flag_t;

static void zs_flag(bool on, void *ctx)
{
    ui_zw_device_page_t page;
    if (!zs_page(&page)) {
        return;
    }
    const ui_zw_device_page_t *pg = &page;
    uint8_t parts = pg->siren.partitions;
    bool burglary = parts != 0u && pg->siren.burglary;
    bool fire = parts != 0u && pg->siren.fire;
    bool squawk = parts != 0u && pg->siren.squawk;
    if (parts == 0u && on) {
        parts = (zs_parts_enabled() & 0x01u) != 0u ? 0x01u : zs_parts_enabled(); /* a new one: 1 */
    }
    switch ((zs_flag_t)(uintptr_t)ctx) {
    case ZS_BURGLARY: burglary = on; break;
    case ZS_FIRE: fire = on; break;
    default: squawk = on; break;
    }
    zs_send(pg, parts, pg->siren.also, burglary, fire, squawk, pg->siren.volume,
            pg->siren.supervision_min);
}

static void zs_chosen(int32_t index)
{
    ui_zw_device_page_t page;
    if (index < 0 || (size_t)index >= s_zs.n || !zs_page(&page)) {
        return;
    }
    const uint16_t v = s_zs.value[index];
    uint8_t parts = page.siren.partitions;
    uint8_t also = page.siren.also;
    uint8_t volume = page.siren.volume;
    uint16_t minutes = page.siren.supervision_min;
    bool burglary = page.siren.burglary;
    bool fire = page.siren.fire;
    switch ((zs_pick_t)s_zs.what) {
    case ZS_PICK_PARTS:
        parts = (uint8_t)v;
        if (parts != 0u && page.siren.partitions == 0u) {
            burglary = true; /* a siren used for the first time sounds for both */
            fire = true;
        }
        break;
    case ZS_PICK_ALSO: also = (uint8_t)v; break;
    case ZS_PICK_VOLUME: volume = (uint8_t)v; break;
    default: minutes = v; break;
    }
    zs_send(&page, parts, also, burglary, fire, page.siren.squawk, volume, minutes);
}

static void zs_pick(void *ctx)
{
    ui_zw_device_page_t page;
    if (!zs_page(&page)) {
        return;
    }
    s_zs.what = (uint8_t)(uintptr_t)ctx;
    s_zs.n = 0;
    const uint8_t all = zs_parts_enabled();
    char words[48];
    switch ((zs_pick_t)s_zs.what) {
    case ZS_PICK_PARTS:
    case ZS_PICK_ALSO: {
        const uint8_t exclude = s_zs.what == ZS_PICK_ALSO ? page.siren.partitions : 0u;
        const uint8_t offer = (uint8_t)(all & (uint8_t)~exclude);
        zs_add(UI_ICON_BELL_OFF, ui_tone_gray(), TR_C("partitions", "None"),
               s_zs.what == ZS_PICK_ALSO ? TR("Only its own partitions")
                                         /* TR: under "None" for a Z-Wave siren's partitions */
                                         : TR("The panel doesn't sound it"),
               0u);
        if ((offer & (uint8_t)(offer - 1u)) != 0u) {
            zs_add(UI_ICON_WAREHOUSE, ui_tone_violet(),
                   exclude != 0u ? TR("All the others") : TR("All partitions"), NULL, offer);
        }
        for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
            if ((offer & (1u << i)) != 0u) {
                I18N_FORMAT(words, sizeof(words), TR("Partition {0}"), (unsigned)(i + 1u));
                zs_add(UI_ICON_WAREHOUSE, ui_tone_violet(), words, NULL, (uint16_t)(1u << i));
            }
        }
        if (s_zs.what == ZS_PICK_ALSO) {
            ui_pick_sheet(TR("Also sounds for"), TR("Other partitions' alarms, never their chirps"),
                          s_zs.choices, s_zs.n, zs_index(page.siren.also), zs_chosen);
        } else {
            ui_pick_sheet(TR_C("siren", "Partitions"), TR("Their alarms and its chirps"),
                          s_zs.choices, s_zs.n, zs_index(page.siren.partitions), zs_chosen);
        }
        break;
    }
    case ZS_PICK_VOLUME: {
        /* TR: a choice: a Z-Wave siren's volume is its own setting */
        zs_add(NULL, ui_tone_blue(), TR("Its own"), TR("As the siren is set"), 0u);
        static const uint8_t VOL[] = {25u, 50u, 75u, 100u};
        for (size_t i = 0; i < sizeof(VOL); i++) {
            /* TR: a Z-Wave siren's volume; {0} a percentage */
            I18N_FORMAT(words, sizeof(words), TR("{0} %"), (unsigned)VOL[i]);
            zs_add(NULL, ui_tone_blue(), words, NULL, VOL[i]);
        }
        ui_pick_sheet(TR("Volume"), TR("For its alarms, chirps and tests"), s_zs.choices, s_zs.n,
                      zs_index(page.siren.volume), zs_chosen);
        break;
    }
    default: {
        static const uint16_t MIN[] = {60u, 120u, 360u, 720u, 1440u};
        /* TR: the default choice of a Z-Wave siren's Supervision */
        zs_add(NULL, ui_tone_blue(), TR("After 4 h"), TR("The default for a Z-Wave siren"), 0u);
        for (size_t i = 0; i < sizeof(MIN) / sizeof(MIN[0]); i++) {
            /* TR: a Z-Wave siren's Supervision choice; {0} hours */
            I18N_FORMAT(words, sizeof(words), TR("After {0} h"), (unsigned)(MIN[i] / 60u));
            zs_add(NULL, ui_tone_blue(), words, NULL, MIN[i]);
        }
        ui_pick_sheet(TR("Supervision"),
                      TR("Not heard this long, it is a siren trouble. It can't be turned off."),
                      s_zs.choices, s_zs.n, zs_index(page.siren.supervision_min), zs_chosen);
        break;
    }
    }
}

static void zs_test(void *ctx)
{
    ui_zw_device_page_t page;
    if (!zs_page(&page)) {
        return;
    }
    (void)ctx;
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_SIREN_TEST;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    req.endpoint = page.siren.endpoint;
    if (request(&req)) {
        ui_toast(TR("Sounding for 2 seconds"));
    }
}

static void device_siren_rows(lv_obj_t *body, const ui_zw_device_page_t *page)
{
    /* TR: a section on a Z-Wave siren's page (BL-037) */
    ui_section(body, TR_C("Z-Wave siren section", "Siren"));
    if (page->siren.fit != UI_ZW_FB_AVAILABLE) {
        ui_paragraph(body,
                     page->siren.fit == UI_ZW_FB_NOT_READY
                         /* TR: a Z-Wave siren whose setup isn't done */
                         ? TR("Finish its setup first: then the panel can sound it with the alarm.")
                         /* TR: a Z-Wave siren the panel can't drive */
                         : TR("Its setup found no way to sound it."),
                     ui_tone_gray());
        return;
    }
    const bool used = page->siren.partitions != 0u;
    const bool can = installer();
    char value[96];
    if (!used) {
        /* TR: a paragraph on a Z-Wave siren's page the panel doesn't sound yet */
        ui_paragraph(body, TR("The panel doesn't sound it yet: turn on what it sounds for."),
                     ui_tone_amber());
    } else if (page->siren.trouble) {
        /* TR: a paragraph on a Z-Wave siren's page in trouble */
        ui_paragraph(body, TR("It isn't answering: a siren trouble. Check its power and signal."),
                     ui_tone_amber());
    }
    lv_obj_t *row = ui_row_toggle_icon(body, UI_ICON_SIREN, ui_tone_red(),
                                       TR_C("siren sounds for", "Burglary and panic"),
                                       TR("A silent panic stays silent"), used && page->siren.burglary,
                                       zs_flag, (void *)(uintptr_t)ZS_BURGLARY);
    if (!can) {
        ui_row_disable(row);
    }
    row = ui_row_toggle_icon(body, UI_ICON_FLAME, ui_tone_red(), TR_C("siren sounds for", "Fire and CO"),
                             page->siren.tones
                                 /* TR: under "Fire and CO" on a Z-Wave siren's page */
                                 ? TR("Its fire tone for fire, its CO tone for CO")
                                 : TR("Its one sound"),
                             used && page->siren.fire, zs_flag, (void *)(uintptr_t)ZS_FIRE);
    if (!can) {
        ui_row_disable(row);
    }
    zs_parts_words(page->siren.partitions, true, value, sizeof(value));
    row = ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR_C("siren", "Partitions"),
                      TR("Their alarms and its chirps"), value, UI_ICON_CHEVRON_RIGHT, zs_pick,
                      (void *)(uintptr_t)ZS_PICK_PARTS);
    if (!can) {
        ui_row_disable(row);
    }
    if (used) {
        zs_parts_words(page->siren.also, false, value, sizeof(value));
        row = ui_row_link(body, UI_ICON_LAYERS, ui_tone_violet(), TR("Also sounds for"),
                          TR("Other partitions' alarms, never their chirps"), value,
                          UI_ICON_CHEVRON_RIGHT, zs_pick, (void *)(uintptr_t)ZS_PICK_ALSO);
        if (!can) {
            ui_row_disable(row);
        }
        if (page->siren.tones) {
            row = ui_row_toggle_icon(body, UI_ICON_BELL, ui_tone_blue(),
                                     TR("Chirp when arming and disarming"),
                                     /* TR: under the chirp switch on a Z-Wave siren's page */
                                     TR("Its arm and disarm tones"), page->siren.squawk, zs_flag,
                                     (void *)(uintptr_t)ZS_SQUAWK);
            if (!can) {
                ui_row_disable(row);
            }
            if (page->siren.volume == 0u) {
                i18n_utf8_copy(value, sizeof(value), TR("Its own"));
            } else {
                I18N_FORMAT(value, sizeof(value), TR("{0} %"), (unsigned)page->siren.volume);
            }
            row = ui_row_link(body, UI_ICON_VOLUME, ui_tone_blue(), TR("Volume"), NULL, value,
                              UI_ICON_CHEVRON_RIGHT, zs_pick, (void *)(uintptr_t)ZS_PICK_VOLUME);
            if (!can) {
                ui_row_disable(row);
            }
        }
        row = ui_row_link(body, UI_ICON_BELL_RING, ui_tone_blue(), TR("Test it"),
                          TR("Sounds it for 2 seconds"), NULL, UI_ICON_CHEVRON_RIGHT, zs_test, NULL);
        if (!can) {
            ui_row_disable(row);
        }
        if (page->siren.supervision_min == 0u) {
            i18n_utf8_copy(value, sizeof(value), TR("After 4 h"));
        } else {
            I18N_FORMAT(value, sizeof(value), TR("After {0} h"),
                        (unsigned)(page->siren.supervision_min / 60u));
        }
        row = ui_row_link(body, UI_ICON_CLOCK_ALERT, ui_tone_amber(), TR("Supervision"),
                          TR("A siren trouble when it isn't heard"), value, UI_ICON_CHEVRON_RIGHT,
                          zs_pick, (void *)(uintptr_t)ZS_PICK_SUPERVISION);
        if (!can) {
            ui_row_disable(row);
        }
    }
}

/* ---- what it switches (ZW-034, ZW-035): its outputs, and a relay that opens a garage door ---- */

static uint32_t outputs_crc(const ui_zw_device_page_t *page)
{
    uint32_t h = 2166136261u; /* FNV-1a over what the rows show */
    const uint8_t *b = (const uint8_t *)page->outputs;
    const size_t n = (size_t)page->output_count * sizeof(page->outputs[0]);
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h ^ page->output_count;
}

static const ui_zw_output_t *output_at(const ui_zw_device_page_t *page, uint8_t endpoint)
{
    for (size_t i = 0; i < page->output_count; i++) {
        if (page->outputs[i].endpoint == endpoint) {
            return &page->outputs[i];
        }
    }
    return NULL;
}

static void output_request(uint8_t kind, uint8_t endpoint, int16_t level, uint16_t zone)
{
    ui_zw_status_t st;
    if (kind != UI_ZW_REQ_GARAGE && read_status(&st) && st.radio != UI_ZW_RADIO_WORKING) {
        ui_toast(TR("The Z-Wave radio isn't working")); /* not the device's fault (B015 UX-049) */
        return;
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    req.endpoint = endpoint;
    req.level = level;
    req.zone = zone;
    if (request(&req)) {
        ui_page_rebuild();
    }
}

/* A switch or relay: tapped, the other way; not known yet: on. */
static void output_toggle(void *ctx)
{
    const uint8_t ep = (uint8_t)(uintptr_t)ctx;
    ui_zw_device_page_t page;
    if (!s_dev.open || !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        return;
    }
    const ui_zw_output_t *o = output_at(&page, ep);
    if (o != NULL && !o->busy) {
        output_request(UI_ZW_REQ_OUTPUT, ep, o->level > 0 ? 0 : 100, 0u);
    }
}

static uint8_t s_level_ep;

static void level_chosen(int32_t index)
{
    static const int16_t LEVELS[] = {0, 25, 50, 75, 100};
    if (index >= 0 && (size_t)index < sizeof(LEVELS) / sizeof(LEVELS[0])) {
        output_request(UI_ZW_REQ_OUTPUT, s_level_ep, LEVELS[index], 0u);
    }
}

static void pick_level(void *ctx)
{
    s_level_ep = (uint8_t)(uintptr_t)ctx;
    static ui_choice_t choices[5];
    static char pct[5][16];
    for (size_t i = 0; i < 5u; i++) {
        choices[i] = (ui_choice_t){NULL, i == 0u ? ui_tone_gray() : ui_tone_blue(), NULL, NULL};
        if (i >= 1u && i <= 3u) {
            /* TR: a light's level in the list of levels; {0} is 25, 50 or 75. At most 15 bytes */
            I18N_FORMAT(pct[i], sizeof(pct[i]), TR_C("percent", "{0} %"), (unsigned)(i * 25u));
            choices[i].title = pct[i];
        }
    }
    choices[0].title = TR_C("light level", "Off");
    choices[4].title = TR_C("light level", "Full");
    int32_t selected = -1; /* its level now, when it is one of these */
    ui_zw_device_page_t page;
    if (s_dev.open && S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        const ui_zw_output_t *o = output_at(&page, s_level_ep);
        static const int8_t AT[5] = {0, 25, 50, 75, 100};
        for (int32_t i = 0; o != NULL && i < 5; i++) {
            selected = o->level == AT[i] ? i : selected;
        }
    }
    ui_pick_sheet(TR("Light level"), NULL, choices, 5u, selected, level_chosen);
}

static void garage_move(void *ctx)
{
    const uintptr_t v = (uintptr_t)ctx;
    output_request(UI_ZW_REQ_GARAGE_MOVE, (uint8_t)(v & 0xFFu), (v >> 8) != 0u ? 100 : 0, 0u);
}

/* Which zone's sensor says where the door is: a relay made a garage door opener. */
static uint8_t s_garage_ep;
static uint16_t s_garage_zones[ALARM_MAX_ZONES] UI_SLOW_BSS;
static size_t s_garage_n;

static void garage_zone_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < s_garage_n) {
        output_request(UI_ZW_REQ_GARAGE, s_garage_ep, 0, s_garage_zones[index]);
    }
}

/* A zone whose sensor can say whether a door is open: a door, window, garage door or plain
 * contact - never smoke, CO, water, motion... (B015 UX-045). */
static bool garage_sensor_zone(uint16_t z)
{
    const alarm_zone_config_t *zc = &zone_cfg()->zones[z - 1u];
    if (zc->type == ALARM_ZT_NOT_USED) {
        return false;
    }
    switch ((alarm_device_class_t)zc->device_class) {
    case ALARM_DEVCLASS_GENERIC:
    case ALARM_DEVCLASS_DOOR:
    case ALARM_DEVCLASS_WINDOW:
    case ALARM_DEVCLASS_GARAGE_DOOR: return true;
    default: return false;
    }
}

static void pick_garage_zone(void *ctx)
{
    s_garage_ep = (uint8_t)(uintptr_t)ctx;
    static ui_choice_t choices[ALARM_MAX_ZONES] UI_SLOW_BSS; /* PSRAM: ~17 KB (B001) */
    static char titles[ALARM_MAX_ZONES][ALARM_NAME_LEN + 8] UI_SLOW_BSS;
    static char details[ALARM_MAX_ZONES][80] UI_SLOW_BSS;
    s_garage_n = 0;
    for (int pass = 0; pass < 2; pass++) { /* its Z-Wave sensors first, then the others */
        for (uint16_t z = 1; z <= ALARM_MAX_ZONES; z++) {
            if (!garage_sensor_zone(z)) {
                continue;
            }
            zone_hub_info_t info;
            memset(&info, 0, sizeof(info));
            const bool bound = S.be.zone_info != NULL && S.be.zone_info(S.be.ctx, z, &info);
            const bool zwave = bound && info.binding.in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_ZWAVE;
            if ((pass == 0) != zwave) {
                continue;
            }
            const size_t i = s_garage_n;
            s_garage_zones[i] = z;
            ui_zone_name(zone_cfg(), z, titles[i], sizeof(titles[i]));
            char dflt[24];
            /* TR: a zone's default name. At most 23 bytes */
            I18N_FORMAT(dflt, sizeof(dflt), TR_C("default zone name", "Zone {0}"), (unsigned)z);
            const bool plain = strcmp(titles[i], dflt) == 0; /* named "Zone 9" already (UX-057) */
            /* TR: after the zone's name in the garage door's list of sensors, when it has no
             * sensor yet. At most 30 bytes */
            const char *no_sensor = TR("no sensor yet");
            /* TR: after the zone's name in the garage door's list of sensors, when its sensor's
             * address isn't known. At most 30 bytes */
            const char *its_sensor = TR("its sensor");
            snprintf(details[i], sizeof(details[i]), "%s%s%.50s", plain ? "" : dflt,
                     plain ? "" : " · ",
                     !bound                  ? no_sensor
                     : info.address[0] != '\0' ? info.address
                                               : its_sensor);
            choices[i] = (ui_choice_t){NULL, ui_tone_blue(), titles[i], details[i]};
            s_garage_n++;
        }
    }
    if (s_garage_n == 0u) {
        ui_toast(TR("No door or tilt sensor zones yet: make its tilt sensor a zone first"));
        return;
    }
    ui_pick_sheet(TR("Garage door's sensor"),
                  TR("The zone whose sensor says whether the door is open. Without its word, the "
                     "door is never moved."),
                  choices, s_garage_n, -1, garage_zone_chosen);
}

static void garage_clear_go(void)
{
    output_request(UI_ZW_REQ_GARAGE, s_garage_ep, 0, 0u);
}

static void garage_clear(void *ctx)
{
    s_garage_ep = (uint8_t)(uintptr_t)ctx;
    ui_confirm(TR("Stop using it for the garage door?"), TR("It becomes a plain relay again."),
               TR_C("confirm button", "Stop"), garage_clear_go);
}

/* A switch's last outcome in words, translated: a row's detail line on a device's page. */
static const char *output_last_words(uint8_t last)
{
    switch (last) {
    case UI_ZW_OUT_ALREADY: return TR("It was there already: nothing was pressed");
    case UI_ZW_OUT_NO_ANSWER: return TR("It didn't confirm: check it");
    case UI_ZW_OUT_SEND_FAILED: return TR("It couldn't be reached: check its power, or the radio");
    case UI_ZW_OUT_NO_POSITION:
        return TR("Nothing pressed: its sensor hasn't said where the door is. Open or close it by "
                  "hand once.");
    case UI_ZW_OUT_NO_MOVE:
        return TR("Its sensor didn't see it get there: check the door and opener");
    case UI_ZW_OUT_STILL_MOVING:
        return TR("Pressed already: wait until its sensor sees it arrive");
    case UI_ZW_OUT_STUCK: return TR("Its relay didn't let go: check it");
    case UI_ZW_OUT_FAILED: return TR("It couldn't be done");
    default: return NULL;
    }
}

static void level_words(const ui_zw_output_t *o, char *out, size_t len)
{
    if (o->busy) {
        snprintf(out, len, NOTR("...")); /* not a word */
    } else if (o->level < 0) {
        /* TR: a switch's state on a device's page, a row's value. At most 15 bytes */
        i18n_utf8_copy(out, len, TR("Not known"));
    } else if (o->kind == UI_ZW_OUT_DIMMER && o->level >= 100) {
        i18n_utf8_copy(out, len, TR_C("light level", "Full"));
    } else if (o->kind == UI_ZW_OUT_DIMMER && o->level > 0) {
        /* TR: a dimmer's level on a device's page, a row's value; {0} is 1 to 99. At most 15
         * bytes */
        I18N_FORMAT(out, len, TR_C("percent", "{0} %"), (int)o->level);
    } else {
        /* TR: a switch's state on a device's page, a row's value. At most 15 bytes */
        i18n_utf8_copy(out, len, o->level > 0 ? TR_C("switch state", "On")
                                              : TR_C("switch state", "Off"));
    }
}

static void device_output_rows(lv_obj_t *body, const ui_zw_device_page_t *page)
{
    if (page->output_count == 0u) {
        return;
    }
    ui_section(body, TR("Controls"));
    for (size_t i = 0; i < page->output_count; i++) {
        const ui_zw_output_t *o = &page->outputs[i];
        void *epctx = (void *)(uintptr_t)o->endpoint;
        const char *last = output_last_words(o->last);
        if (o->garage) {
            /* where its sensor says it is - never "opening" on the strength of a press alone
             * (B015 UX-043); the buttons stay in place (UX-047) */
            /* TR: a garage door's state from its sensor, the value of its row */
            const char *state = strcmp(o->garage_state, "open") == 0 ? TR_C("garage door", "Open")
                                : strcmp(o->garage_state, "closed") == 0
                                    ? TR_C("garage door", "Closed")
                                    : TR_C("garage door", "Unknown");
            const char *detail = last;
            if (o->garage_goal != 0u && o->last != UI_ZW_OUT_STILL_MOVING) {
                detail = o->garage_goal == 1u ? TR("Pressed to open: waiting for its sensor")
                                              : TR("Pressed to close: waiting for its sensor");
            }
            ui_row_link(body, UI_ICON_WAREHOUSE,
                        last != NULL && o->garage_goal == 0u ? ui_tone_amber() : ui_tone_blue(),
                        TR("Garage door"), detail, state, NULL, NULL, NULL);
            ui_row_action(body, UI_ICON_LOCK_OPEN, TR("Open it"), ui_tone_blue(), garage_move,
                          (void *)(uintptr_t)(1u << 8 | o->endpoint));
            ui_row_action(body, UI_ICON_LOCK, TR("Close it"), ui_tone_blue(), garage_move,
                          (void *)(uintptr_t)o->endpoint);
            char opener[64];
            char zname[ALARM_NAME_LEN + 8];
            ui_zone_name(zone_cfg(), o->garage_zone, zname, sizeof(zname));
            /* TR: a row's detail line on a device's page; {0} is the relay's number, {1} the
             * name of the zone whose sensor says where the door is */
            I18N_FORMAT(opener, sizeof(opener),
                        TR("Relay {0} · sensor {1} · tap to stop using it"),
                        (unsigned)o->endpoint, zname);
            ui_row_link(body, UI_ICON_SETTINGS, ui_tone_gray(), TR("Garage door opener"), opener,
                        NULL, UI_ICON_CHEVRON_RIGHT, garage_clear, epctx);
            continue;
        }
        char title[24];
        char value[16];
        level_words(o, value, sizeof(value));
        if (o->kind == UI_ZW_OUT_RELAY) {
            /* TR: a row's title on a device's page; {0} is the relay's number. At most 20
             * bytes */
            I18N_FORMAT(title, sizeof(title), TR("Relay {0}"), (unsigned)o->endpoint);
        } else {
            /* TR: a row's title on a device's page: a light or a switch it controls. At most 23
             * bytes */
            i18n_utf8_copy(title, sizeof(title),
                           o->kind == UI_ZW_OUT_DIMMER ? TR_C("output kind", "Light")
                                                       : TR_C("output kind", "Switch"));
        }
        const bool dimmer = o->kind == UI_ZW_OUT_DIMMER;
        ui_row_link(body, UI_ICON_ZAP, last != NULL ? ui_tone_amber() : ui_tone_blue(), title,
                    last != NULL ? last
                    : o->busy    ? TR("Setting it...")
                    : dimmer     ? TR("Tap to set its level")
                    : o->level > 0 ? TR("Tap to switch it off")
                                   : TR("Tap to switch it on"),
                    value, dimmer && !o->busy ? UI_ICON_CHEVRON_RIGHT : NULL,
                    o->busy ? NULL : dimmer ? pick_level : output_toggle, epctx);
        if (o->kind == UI_ZW_OUT_RELAY && installer()) {
            char use[40];
            /* TR: a row's title on a device's page; {0} is the relay's number. At most 36
             * bytes */
            I18N_FORMAT(use, sizeof(use), TR("Use relay {0} for a garage door"),
                        (unsigned)o->endpoint);
            ui_row_link(body, UI_ICON_WAREHOUSE, ui_tone_gray(), use,
                        TR("With a tilt sensor that says where the door is"), NULL,
                        UI_ICON_CHEVRON_RIGHT, pick_garage_zone, epctx);
        }
    }
}

/* ---- its software (ZW-033): checked and installed only when the installer asks ---- */

static void wake_words_of(const ui_zw_device_page_t *page, char *out, size_t len);

static void fw_request(uint8_t kind)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = S.auth_user;
    req.node = s_dev.node;
    req.generation = s_dev.generation;
    if (request(&req)) {
        ui_page_rebuild();
    }
}

static void fw_check(void *ctx)
{
    (void)ctx;
    s_fwres.result = UI_ZW_RES_NONE;
    fw_request(UI_ZW_REQ_FW_CHECK);
}

static void fw_install_go(void)
{
    s_fwres.result = UI_ZW_RES_NONE;
    fw_request(UI_ZW_REQ_FW_INSTALL);
}

static void fw_install(void *ctx)
{
    (void)ctx;
    ui_zw_device_page_t page;
    if (!installer() || !s_dev.open ||
        !S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page) ||
        page.fw.check != UI_ZW_FW_AVAILABLE) {
        return;
    }
    static char title[48];
    /* TR: a confirmation's title; {0} is a version like 2.1. At most 30 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Install version {0}?"), page.fw.version);
    ui_confirm(title,
               page.dev.listening && page.dev.power == UI_ZW_POWER_BATTERY
                   ? TR("It takes several minutes and it may not report meanwhile. Keep its "
                        "batteries in until it's done.")
               : page.dev.listening
                   ? TR("It takes several minutes and it may not report meanwhile. Keep its power "
                        "on until it's done.")
                   : TR("It takes several minutes: wake it when asked and keep it near the panel. "
                        "It may not report meanwhile."),
               TR("Install"), fw_install_go);
}

static void fw_stop_go(void)
{
    ui_zw_progress_t p;
    read_progress(&p);
    ui_zw_device_page_t page;
    const bool downloading = s_dev.open &&
                             S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page) &&
                             page.fw.check == UI_ZW_FW_DOWNLOADING;
    if (!downloading && (p.op != UI_ZW_OP_FIRMWARE || p.step == UI_ZW_STEP_DONE)) {
        ui_toast(TR("It had already finished")); /* UX-033 */
        return;
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_STOP;
    req.user = S.auth_user;
    request(&req);
}

static void fw_stop(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Stop the update?"), TR("It keeps its old software."), TR("Stop it"),
               fw_stop_go);
}

/* Why a device's update ended without it, translated (a paragraph on its page). */
static const char *fw_failure_words(uint8_t why)
{
    switch (why) {
    case UI_ZW_FWF_NOT_UPGRADABLE: return TR("It says its software can't be changed.");
    case UI_ZW_FWF_WRONG_DEVICE:
        return TR("It isn't the device that file is for. Nothing changed.");
    case UI_ZW_FWF_LOW_BATTERY:
        return TR("It refused: its battery is low. Replace it, then try again.");
    case UI_ZW_FWF_REFUSED: return TR("It refused the new software and keeps its old one.");
    case UI_ZW_FWF_BAD_TRANSFER:
        return TR("The software arrived damaged, so it kept its old one. Try again.");
    case UI_ZW_FWF_NO_ANSWER:
        return TR("It stopped answering during the update. It usually keeps its old software: "
                  "check it works, then try again.");
    case UI_ZW_FWF_NOT_WOKEN: return TR("It wasn't woken in time. Nothing was sent: try again.");
    case UI_ZW_FWF_SEND_FAILED:
        return TR("The radio couldn't reach it. Bring it closer, then try again.");
    case UI_ZW_FWF_STOPPED: return TR("Stopped. It keeps its old software.");
    case UI_ZW_FWF_NO_RADIO:
        return TR("The Z-Wave radio stopped during the update. Check the device works, then try "
                  "again.");
    default:
        return TR("It couldn't store or start the new software. It usually keeps its old one: "
                  "check it works.");
    }
}

/* The running update in words: waiting for a sleeper, starting, or how far. */
static void fw_progress_words(const ui_zw_progress_t *p, const ui_zw_device_page_t *page,
                              char *out, size_t len)
{
    if (p->fw_wake) {
        char words[UI_ZW_WORDS_LEN];
        wake_words_of(page, words, sizeof(words));
        const unsigned min = (unsigned)((p->seconds_left + 59u) / 60u);
        /* TR: a paragraph on a device's page while its update waits; {0} is what to do to wake
         * it, {1} the minutes left (1 to 60) */
        I18N_FORMAT(out, len,
                    TRN("Wake it now: {0}. The update starts when it does ({1} min left).",
                        "Wake it now: {0}. The update starts when it does ({1} min left).", min),
                    words, min);
    } else if (p->fw_total == 0u) {
        /* TR: a paragraph on a device's page while its update starts. At most 159 bytes */
        i18n_utf8_copy(out, len, TR("Starting the update..."));
    } else {
        const unsigned pct = (unsigned)((uint32_t)p->fw_done * 100u / p->fw_total);
        /* TR: a paragraph on a device's page while its update runs; {0} is 0 to 100 */
        I18N_FORMAT(out, len, TR("Sending its new software: {0} %"), pct);
    }
}

static void device_software_rows(lv_obj_t *body, const ui_zw_device_page_t *page, void *devctx)
{
    const ui_zw_device_t *d = &page->dev;
    ui_zw_progress_t p;
    read_progress(&p);
    const bool running = p.op == UI_ZW_OP_FIRMWARE && p.node == d->node &&
                         p.generation == d->generation && p.step != UI_ZW_STEP_DONE &&
                         p.step != UI_ZW_STEP_IDLE;
    s_dev.fw_running = running;
    s_dev.fw_line = NULL;
    if (!page->fw.known && !running) {
        return; /* its setup hasn't read what it runs */
    }
    ui_section(body, TR("Software"));
    char version[40];
    if (page->fw.has_hardware) {
        /* TR: a row's value on a device's page: the software's version, then its hardware's;
         * {0} and {1} make a version like 2.1, {2} is a number. At most 24 bytes */
        I18N_FORMAT(version, sizeof(version), TR("{0}.{1} · hardware {2}"),
                    (unsigned)page->fw.major, (unsigned)page->fw.minor,
                    (unsigned)page->fw.hardware);
    } else {
        snprintf(version, sizeof(version), "%u.%u", (unsigned)page->fw.major,
                 (unsigned)page->fw.minor);
    }
    ui_row_value(body, TR("Version"), version);
    if (running) {
        char text[160];
        fw_progress_words(&p, page, text, sizeof(text));
        s_dev.fw_line = ui_paragraph(body, text, ui_tone_blue());
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop the update"), ui_tone_red(), fw_stop,
                      devctx);
        return;
    }
    if (s_fwres.result != UI_ZW_RES_NONE && s_fwres.node == d->node &&
        s_fwres.generation == d->generation) {
        if (s_fwres.result == UI_ZW_RES_FW_UPDATED &&
            (page->fw.major != s_fwres.major || page->fw.minor != s_fwres.minor)) {
            char text[64];
            /* TR: a paragraph on a device's page after an update; {0}.{1} is a version. At most
             * 50 bytes */
            I18N_FORMAT(text, sizeof(text), TR("Updated: it runs version {0}.{1} now."),
                        (unsigned)page->fw.major, (unsigned)page->fw.minor);
            ui_paragraph(body, text, ui_tone_green());
        } else if (s_fwres.result == UI_ZW_RES_FW_UPDATED) {
            ui_paragraph(body, TR("It took its new software. It restarts, then the panel reads "
                                  "it again."),
                         ui_tone_green());
        } else {
            ui_paragraph(body, fw_failure_words(s_fwres.why), ui_tone_amber());
        }
    }
    if (!page->fw.updatable) {
        ui_paragraph(body,
                     d->state == UI_ZW_DEV_READY || d->state == UI_ZW_DEV_NO_REPORTS
                         ? TR("It doesn't take software updates by radio.")
                         : TR("Its software can be checked once its setup finishes."),
                     ui_tone_gray());
        return;
    }
    switch (page->fw.check) {
    case UI_ZW_FW_CHECKING:
        ui_paragraph(body, TR("Asking the update service..."), ui_tone_blue());
        return;
    case UI_ZW_FW_DOWNLOADING:
        ui_paragraph(body, TR("Downloading its new software and checking it..."), ui_tone_blue());
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop the update"), ui_tone_red(), fw_stop,
                      devctx);
        return;
    case UI_ZW_FW_AVAILABLE: {
        char name[48];
        /* TR: a row's title on a device's page; {0} is a version like 2.1; the second is a
         * test version. At most 28 bytes */
        I18N_FORMAT(name, sizeof(name),
                    page->fw.beta ? TR("Install version {0} (test)") : TR("Install version {0}"),
                    page->fw.version);
        ui_row_link(body, UI_ICON_SPARKLES, ui_tone_blue(), name,
                    page->fw.words[0] != '\0' ? page->fw.words : TR("Its changes aren't described"),
                    NULL, UI_ICON_CHEVRON_RIGHT, fw_install, devctx);
        return;
    }
    case UI_ZW_FW_UP_TO_DATE: ui_row_value(body, TR("Updates"), TR("Up to date")); break;
    case UI_ZW_FW_UNKNOWN:
        ui_paragraph(body, TR("The update service doesn't know this device."), ui_tone_gray());
        break;
    case UI_ZW_FW_CHECK_FAILED:
    case UI_ZW_FW_FILE_FAILED: ui_paragraph(body, page->fw.words, ui_tone_amber()); break;
    default: break;
    }
    const bool after = s_fwres.result == UI_ZW_RES_FW_FAILED && s_fwres.node == d->node &&
                       s_fwres.generation == d->generation;
    ui_row_action(body, UI_ICON_REFRESH,
                  page->fw.check == UI_ZW_FW_NONE && !after ? TR("Check for an update")
                                                            : TR("Check again"),
                  ui_tone_blue(), fw_check, devctx);
    if (page->fw.check == UI_ZW_FW_NONE) {
        ui_paragraph(body, TR("Checked only when you ask; nothing installs by itself."),
                     ui_tone_gray());
    }
}

/* A rebuilt page keeps its scroll, but a removal's running line and its outcome sit at the top:
 * show them (ZW-029; no animation). */
static void scroll_page_top(void)
{
    if (S.page_body != NULL) {
        lv_obj_scroll_to_y(S.page_body, 0, LV_ANIM_OFF);
    }
}

static void remove_failed_go(void)
{
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)s_confirm_arg, &node, &generation);
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_REMOVE_FAILED;
    req.user = S.auth_user;
    req.node = node;
    req.generation = generation;
    s_clean.result = UI_ZW_RES_NONE;
    if (request(&req)) {
        ui_page_rebuild(); /* "Checking it no longer answers..." on its page */
        scroll_page_top();
    }
}

static void remove_failed_confirm(void *ctx)
{
    s_confirm_arg = ctx;
    static char text[200];
    /* TR: a confirmation's text. At most 199 bytes */
    i18n_utf8_copy(text, sizeof(text), TR("The radio checks it no longer answers first. One that "
                                          "answers is kept: remove it the normal way."));
    uint16_t node, generation;
    device_args((int32_t)(intptr_t)ctx, &node, &generation);
    ui_zw_device_page_t page;
    if (S.be.zw_device(S.be.ctx, node, generation, &page)) {
        for (size_t i = 0; i < page.output_count; i++) {
            if (page.outputs[i].garage) { /* B015 UX-050 */
                /* TR: a confirmation's text; {0} is the relay's number. At most 190 bytes */
                I18N_FORMAT(text, sizeof(text),
                            TR("Relay {0} opens the garage door: that stops. The radio checks it "
                               "no longer answers first."),
                            (unsigned)page.outputs[i].endpoint);
            }
        }
    }
    ui_confirm(TR("Remove it: it's reset, broken or gone?"), text, TR("Remove it"),
               remove_failed_go);
}

/* The removal outcome of the device on its page, in words (screens.md §6). */
static void clean_words(uint8_t result, lv_obj_t *body)
{
    switch (result) {
    case UI_ZW_RES_STILL_ANSWERS:
        ui_paragraph(body,
                     TR("It answered, so it was kept. Use Remove this device and press its "
                        "button."),
                     ui_tone_amber());
        break;
    case UI_ZW_RES_REFUSED:
        ui_paragraph(body,
                     TR("Not removed: the Z-Wave radio doesn't count it as failed yet. If it "
                        "was reset or has no power, wait a minute and try again."),
                     ui_tone_amber());
        break;
    case UI_ZW_RES_TIMED_OUT:
        ui_paragraph(body,
                     TR("Not confirmed: the Z-Wave radio didn't finish in time. It is still "
                        "listed: try again."),
                     ui_tone_amber());
        break;
    default:
        ui_paragraph(body,
                     TR("Not removed: the Z-Wave radio couldn't do it. Nothing changed: try "
                        "again."),
                     ui_tone_amber());
        break;
    }
}

/* Removing it, both ways (screens.md §6): the normal way needs the device; the other checks it
 * no longer answers and only then takes it out of the radio's list. */
static void device_remove_rows(lv_obj_t *body, const ui_zw_device_t *d, void *devctx)
{
    ui_section(body, TR("Removing it"));
    if (s_dev.cleaning) {
        return; /* the page's top says the check runs */
    }
    ui_row_link(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove this device"),
                TR("While it works: press its button when asked"), NULL, UI_ICON_CHEVRON_RIGHT,
                open_remove_for, devctx);
    ui_row_link(body, UI_ICON_CIRCLE_X, ui_tone_red(), TR("Remove it: it's reset, broken or gone"),
                TR("The radio checks it no longer answers first"), NULL, UI_ICON_CHEVRON_RIGHT,
                remove_failed_confirm, devctx);
    if (!d->listening) {
        ui_paragraph(body,
                     TR("A sleeping sensor can't answer that check: use the second way only if "
                        "it was reset or is really gone."),
                     ui_tone_gray());
    }
    ui_zw_device_page_t page;
    if (S.be.zw_device(S.be.ctx, d->node, d->generation, &page)) {
        for (size_t i = 0; i < page.output_count; i++) {
            if (page.outputs[i].garage) {
                char text[96];
                /* TR: a paragraph on a device's page; {0} is the relay's number. At most 90
                 * bytes */
                I18N_FORMAT(text, sizeof(text),
                            TR("Relay {0} opens the garage door: removing it stops that."),
                            (unsigned)page.outputs[i].endpoint);
                ui_paragraph(body, text, ui_tone_amber());
            }
        }
    }
}

/* The quirk's own words start a sentence elsewhere ("Click the Z-Wave button..."); here they
 * follow "wake it: " mid-sentence, so their first letter reads lower-case. */
static void wake_words_of(const ui_zw_device_page_t *page, char *out, size_t len)
{
    /* TR: what to do to wake a device, in the middle of a sentence ("Wake it now: ..."), so it
     * starts with a small letter. At most 100 bytes */
    if (page->wake_words[0] != '\0') {
        /* the quirks table's words (zw_quirks.c, marked "wake up"), in this screen's language,
         * the first letter made small to go on after the colon */
        i18n_utf8_copy(out, len, TR_C("wake up", page->wake_words));
        if (out[0] >= 'A' && out[0] <= 'Z') {
            out[0] = (char)(out[0] - 'A' + 'a');
        }
    } else {
        i18n_utf8_copy(out, len, TR("see its manual, usually by pressing its button"));
    }
}

/* A device's setup while it runs: its first, tried again, or read again (BL-152). */
static void setting_up_words(const ui_zw_device_page_t *page, char *text, size_t len)
{
    const ui_zw_device_t *d = &page->dev;
    if (rereading(d)) {
        if (d->state != UI_ZW_DEV_WAKE_NEEDED) {
            /* TR: a paragraph on a device's page while its setup is read again. At most 120
             * bytes */
            i18n_utf8_copy(text, len, TR("Reading it again. Its zones keep working."));
        } else {
            char words[UI_ZW_WORDS_LEN];
            wake_words_of(page, words, sizeof(words));
            /* TR: a paragraph on a device's page while its setup waits to be read again; {0} is
             * what to do to wake it. At most 190 bytes */
            I18N_FORMAT(text, len,
                        TR("It's read again at its next wake-up. Its zones keep working. To "
                           "start now, wake it ({0})."),
                        words);
        }
        return;
    }
    /* TR: a paragraph on a device's page while its setup runs; {0} of {1} steps done. At most
     * 63 bytes */
    I18N_FORMAT(text, len,
                s_retry.node == d->node && s_retry.generation == d->generation
                    ? TR("Trying its setup again: step {0} of {1}.")
                    : TR("Setting it up: step {0} of {1}."),
                (unsigned)d->setup_done, (unsigned)d->setup_total);
}

static void build_device(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    uint16_t node, generation;
    device_args(arg, &node, &generation);
    s_dev.node = node;
    s_dev.generation = generation;
    s_dev.last_heard = NULL;
    s_dev.signal = NULL;
    s_dev.check_in = NULL;
    s_dev.open = false;
    ui_zw_device_page_t page;
    if (S.be.zw_device == NULL || !S.be.zw_device(S.be.ctx, node, generation, &page)) {
        /* TR: a page's title when its device has left the network */
        i18n_utf8_copy(title, tlen, TR("Z-Wave device"));
        ui_paragraph(body, TR("This device is no longer in the network."), ui_tone_gray());
        return;
    }
    const ui_zw_device_t *d = &page.dev;
    char name[64];
    device_name(d, name, sizeof(name));
    i18n_utf8_copy(title, tlen, name);
    /* TR: the line under a device page's title; {0} is its model or what it is, {1} its number */
    I18N_FORMAT(note, nlen, TR("{0} · device {1}"),
                d->model[0] != '\0' ? d->model : kind_words(d->kind), (unsigned)d->node);
    const uint32_t packed = (uint32_t)node | (uint32_t)generation << 16;
    void *devctx = (void *)(intptr_t)(int32_t)packed;
    s_dev.setup = NULL;
    {
        ui_zw_progress_t p; /* a "reset, broken or gone" removal of it running now */
        read_progress(&p);
        s_dev.cleaning = p.op == UI_ZW_OP_REMOVE_FAILED && p.node == node &&
                         p.generation == generation && p.step != UI_ZW_STEP_DONE &&
                         p.step != UI_ZW_STEP_IDLE;
    }
    /* A removal as failed: running, or how it ended - at the top, where a rebuilt page is seen */
    if (s_dev.cleaning) {
        ui_paragraph(body, TR("Checking that it no longer answers..."), ui_tone_blue());
    } else if (s_clean.result != UI_ZW_RES_NONE && s_clean.node == node &&
               s_clean.generation == generation) {
        clean_words(s_clean.result, body);
    }
    const bool reread = rereading(d);
    if (d->state == UI_ZW_DEV_WAKE_NEEDED && !reread) {
        char words[UI_ZW_WORDS_LEN];
        wake_words_of(&page, words, sizeof(words));
        char text[160];
        /* TR: a paragraph on a device's page; {0} is what to do to wake it. At most 130 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Wake it to finish setting it up: {0}."), words);
        ui_paragraph(body, text, ui_tone_amber());
    } else if (d->state == UI_ZW_DEV_JOINING) {
        ui_paragraph(body,
                     TR("It's being added: its security question waits on the Add a device "
                        "page."),
                     ui_tone_amber());
        if (installer()) {
            ui_row_link(body, UI_ICON_PLUS, ui_tone_blue(), TR("Answer it"), TR("Add a device"),
                        NULL,
                        UI_ICON_CHEVRON_RIGHT, open_page, (void *)(intptr_t)PAGE_ZWAVE_ADD);
        }
    } else if (d->state == UI_ZW_DEV_SETTING_UP || d->state == UI_ZW_DEV_SETUP_STOPPED ||
               d->state == UI_ZW_DEV_WAKE_NEEDED) {
        /* the same two rows whether it runs or stopped: the page doesn't jump when a retry
         * starts and stops again (B015 UX-008). A re-read (BL-152, the panel says it: all its
         * steps done) is calm: the device works on as before */
        const bool again = s_retry.node == node && s_retry.generation == generation;
        const bool running = d->state != UI_ZW_DEV_SETUP_STOPPED;
        char text[192];
        if (running) {
            setting_up_words(&page, text, sizeof(text));
        } else if (reread && d->listening) {
            /* TR: a paragraph on a device's page when reading its setup again stopped. At most
             * 150 bytes */
            i18n_utf8_copy(text, sizeof(text),
                           TR("Reading it again stopped: it didn't answer. It works as before. "
                              "Bring it closer or check its power, then try again."));
        } else if (reread) {
            char words[UI_ZW_WORDS_LEN];
            wake_words_of(&page, words, sizeof(words));
            /* TR: a paragraph on a device's page when reading its setup again stopped; {0} is
             * what to do to wake it. At most 120 bytes */
            I18N_FORMAT(text, sizeof(text),
                        TR("Reading it again stopped. It works as before. Wake it ({0}), then "
                           "try again."),
                        words);
        } else if (d->listening) {
            /* TR: a paragraph on a device's page when its setup stopped. At most 150 bytes */
            i18n_utf8_copy(text, sizeof(text),
                           again ? TR("Tried again. Its setup stopped: it didn't answer. Bring it "
                                      "closer or check its power, then try again. It can't be "
                                      "used until then.")
                                 : TR("Its setup stopped: it didn't answer. Bring it closer or "
                                      "check its power, then try again. It can't be used until "
                                      "then."));
        } else {
            char words[UI_ZW_WORDS_LEN];
            wake_words_of(&page, words, sizeof(words));
            /* TR: a paragraph on a device's page when its setup stopped; {0} is what to do to
             * wake it. At most 120 bytes */
            I18N_FORMAT(text, sizeof(text),
                        again ? TR("Tried again. Its setup stopped. Wake it ({0}), then try "
                                   "again.")
                              : TR("Its setup stopped. Wake it ({0}), then try again."),
                        words);
        }
        lv_obj_t *para = ui_paragraph(body, text, running || reread ? ui_tone_blue()
                                                                     : ui_tone_amber());
        if (running) {
            s_dev.setup = para;
        }
        if (installer() && !s_dev.cleaning && !(reread && running)) {
            ui_row_action(body, UI_ICON_REFRESH, TR("Try its setup again"), ui_tone_blue(),
                          setup_again, devctx);
        }
    }
    if (d->unreachable && (d->state != UI_ZW_DEV_SETUP_STOPPED || reread) && d->listening) {
        /* one box at a time: a device that answers when called doesn't "check in" (UX-007) */
        ui_paragraph(body,
                     TR("It stopped answering. Check its power or batteries, or bring it "
                        "closer. If it was reset, remove it below."),
                     ui_tone_amber());
    } else if (d->unreachable && (d->state != UI_ZW_DEV_SETUP_STOPPED || reread)) {
        char words[UI_ZW_WORDS_LEN];
        wake_words_of(&page, words, sizeof(words));
        char text[192];
        /* TR: a paragraph on a device's page; {0} is what to do to wake it. At most 150 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TR("It missed its check-ins. Change its battery or bring it closer, then wake "
                       "it: {0}."),
                    words);
        ui_paragraph(body, text, ui_tone_amber());
    }
    ui_row_value(body, TR("Security"), security_full(d->security));
    /* its power as known, never guessed from its radio (a keypad woken by the panel's call runs
     * on batteries) */
    if (d->battery_pct != UI_ZW_BATTERY_UNKNOWN) {
        char pct[16];
        /* TR: a row's value on a device's page: its battery's charge; {0} is 0 to 100. At most
         * 15 bytes */
        I18N_FORMAT(pct, sizeof(pct), TR_C("percent", "{0} %"), (unsigned)d->battery_pct);
        ui_row_value(body, TR("Battery"), pct);
    } else if (d->power == UI_ZW_POWER_MAINS) {
        ui_row_value(body, TR("Battery"), TR("Mains powered"));
    } else if (d->power == UI_ZW_POWER_BATTERY) {
        ui_row_value(body, TR("Battery"), TR("Not reported yet"));
    } else {
        ui_row_value(body, TR("Battery"), TR_C("battery", "Unknown"));
    }
    char heard[40];
    if (d->heard_s == UI_ZW_NEVER) {
        /* TR: a row's value on a device's page. At most 39 bytes */
        i18n_utf8_copy(heard, sizeof(heard), TR("Not heard yet"));
    } else {
        char span[24];
        span_words(d->heard_s, span, sizeof(span));
        /* TR: a row's value on a device's page; {0} is how long, like "45 min" or "2 h 30 min".
         * At most 33 bytes */
        I18N_FORMAT(heard, sizeof(heard), TR("{0} ago"), span);
    }
    s_dev.last_heard = ui_row_value(body, TR("Last heard"), heard);
    if (d->signal != UI_ZW_SIGNAL_UNKNOWN) {
        s_dev.signal = ui_row_value(body, TR_C("Z-Wave device", "Signal"), signal_words(d->signal));
    }
    if (page.jam_report_s != UI_ZW_NEVER && page.jam_report_s < 24u * 3600u) {
        /* network.md §9: its own view of the air (the Z-Wave page's "Interference" is the radio's) */
        char span[24];
        char jam[48];
        span_words(page.jam_report_s, span, sizeof(span));
        /* TR: a row's value on a device's page; {0} is how long, like "45 min". At most 38
         * bytes */
        I18N_FORMAT(jam, sizeof(jam), TR("Reported {0} ago"), span);
        ui_row_value(body, TR("Interference"), jam);
    }
    if (installer()) {
        if (d->listening) { /* it answers at any time: nothing to check in for */
            ui_row_value(body, TR("Checks in"),
                         d->flirs ? TR("Not needed: it answers when called") : TR("Not needed"));
        } else {
            /* network.md §7: a device that checks in, once it is set up, takes a check-in of its
             * own at its next wake-up; "Never" (it wakes only by hand) is shown, not changed */
            char ci[32];
            char next[80];
            uint32_t offered[CHECK_IN_CHOICES];
            check_in_words(page.check_in_s, ci, sizeof(ci));
            if (!page.check_in_known) {
                /* TR: a row's value on a device's page. At most 31 bytes */
                i18n_utf8_copy(ci, sizeof(ci), TR("Not known yet")); /* not read yet (UX-003) */
            }
            check_in_next_words(&page, next, sizeof(next));
            const bool can = page.check_in_known && page.check_in_s != 0u &&
                             d->state != UI_ZW_DEV_SETTING_UP && d->state != UI_ZW_DEV_JOINING &&
                             d->state != UI_ZW_DEV_SETUP_STOPPED &&
                             d->state != UI_ZW_DEV_WAKE_NEEDED &&
                             d->state != UI_ZW_DEV_NOT_USABLE &&
                             check_in_choices(&page, offered) > 0u;
            s_dev.check_in = ui_row_link(body, UI_ICON_CLOCK, can ? ui_tone_blue() : ui_tone_gray(),
                                         TR("Checks in"), next, ci,
                                         can ? UI_ICON_CHEVRON_RIGHT : NULL,
                                         can ? pick_device_check_in : NULL, NULL);
        }
        if (d->zone_count > 0u) {
            for (uint8_t i = 0; i < d->zone_count && i < UI_ZW_ZONES_MAX; i++) {
                char zname[ALARM_NAME_LEN + 8];
                char zdefault[48];
                char zvalue[sizeof(zdefault) + sizeof(zname) + 8]; /* "Zone 12 · Kitchen" */
                ui_zone_name(zone_cfg(), d->zones[i], zname, sizeof(zname));
                I18N_FORMAT(zdefault, sizeof(zdefault), TR_C("default zone name", "Zone {0}"),
                            (unsigned)d->zones[i]);
                if (strcmp(zname, zdefault) == 0) { /* still its default name: don't say it twice */
                    i18n_utf8_copy(zvalue, sizeof(zvalue), zname);
                } else {
                    snprintf(zvalue, sizeof(zvalue), "%s · %s", zdefault, zname);
                }
                ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Used by"), NULL, zvalue,
                           UI_ICON_CHEVRON_RIGHT, open_zone_row,
                           (void *)(intptr_t)(int32_t)d->zones[i]);
            }
        } else if ((d->state == UI_ZW_DEV_READY || reread) && page.input_count > 0u) {
            ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("Used by"), NULL,
                        TR("Make it a zone"),
                       UI_ICON_CHEVRON_RIGHT, make_it_a_zone, devctx);
        } else if (d->state != UI_ZW_DEV_READY && !reread) {
            /* nothing to offer until its setup has read what it can do */
            ui_row_link(body, UI_ICON_TAG, ui_tone_gray(), TR("Used by"),
                        TR("Finish its setup first"), TR("Nothing yet"), NULL, NULL, NULL);
        }
        device_output_rows(body, &page);
        if (page.feedback.fit != UI_ZW_FB_NONE || page.control.fit != UI_ZW_FB_NONE) {
            device_feedback_rows(body, &page);
        }
        if (page.siren.fit != UI_ZW_FB_NONE) {
            device_siren_rows(body, &page); /* OUT-016 (BL-037) */
        }
        device_software_rows(body, &page, devctx);
        if (d->listening && !s_dev.fw_running) {
            /* any device that answers at once (BL-152): its answer is the toast, as for one that
             * stopped answering */
            ui_row_action(body, UI_ICON_REFRESH, TR("Check it now"), ui_tone_blue(), ping_go,
                          devctx);
        }
        if (installer() && !s_dev.fw_running && !s_dev.cleaning &&
            (d->state == UI_ZW_DEV_READY || d->state == UI_ZW_DEV_NO_REPORTS)) {
            /* BL-152: read what it can do again (Z-Wave JS's re-interview), its zones kept */
            ui_row_link(body, UI_ICON_REFRESH, ui_tone_blue(), TR("Set it up again"),
                        TR("Read what it can do again: its zones keep working"), NULL, NULL,
                        reread_ask, devctx);
        }
        if (installer() && d->unreachable && d->node < 256u) {
            /* §10: broken or lost - a new one in its place, its zones kept */
            ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_blue(), TR("Replace it"),
                        TR("A new device takes its place: its zones follow it"), NULL,
                        UI_ICON_CHEVRON_RIGHT, replace_ask, devctx);
        }
        if (installer() && d->state == UI_ZW_DEV_READY) {
            ui_row_link(body, UI_ICON_WAVES, ui_tone_blue(), TR("Rebuild its routes"),
                        d->listening ? TR("Refresh how its signals reach the panel")
                                     : TR("Tap, then press its button to wake it"),
                        NULL, NULL, routes_one_go, devctx);
        }
        if (page.identify && !d->unreachable) {
            /* which one is it: its own light blinks 3 times (the panel's screen never does) */
            ui_row_link(body, UI_ICON_SPARKLES, ui_tone_blue(), TR("Blink its light"),
                        TR("Find which device this is"), NULL, NULL, identify_go, devctx);
        }
        if (!s_dev.fw_running) {
            device_remove_rows(body, d, devctx); /* not while it gets new software (UX-037) */
        }
    }
    s_dev.unreachable = d->unreachable;
    s_dev.state = d->state;
    s_dev.security = d->security;
    s_dev.fb_fit = page.feedback.fit;
    s_dev.fb_partition = page.feedback.partition;
    s_dev.fb_live = page.feedback.live;
    s_dev.kc_fit = page.control.fit;
    s_dev.kc_partition = page.control.partition;
    s_dev.kc_panic = page.control.panic_mask;
    s_dev.kc_live = page.control.live;
    s_dev.fw_check = page.fw.check;
    s_dev.outputs_crc = outputs_crc(&page);
    s_dev.fw_major = page.fw.major;
    s_dev.fw_minor = page.fw.minor;
    s_dev.open = true;
    s_dev.polled = lv_tick_get();
}

static void device_tick(void)
{
    if (!s_dev.open || lv_tick_elaps(s_dev.polled) < POLL_MS) {
        return;
    }
    s_dev.polled = lv_tick_get();
    /* The unreachable banner and its extra rows, and the "still interviewing" one, are structural
     * (screens.md §6): a real change of state rebuilds the page rather than editing words that
     * were never drawn. Checked before the ping/removal replies below, which may also rebuild. */
    {
        ui_zw_device_page_t page;
        if (S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page) &&
            (page.dev.unreachable != s_dev.unreachable || page.dev.state != s_dev.state ||
             page.dev.security != s_dev.security || page.feedback.fit != s_dev.fb_fit ||
             page.feedback.partition != s_dev.fb_partition ||
             page.feedback.live != s_dev.fb_live || page.control.fit != s_dev.kc_fit ||
             page.control.partition != s_dev.kc_partition ||
             page.control.panic_mask != s_dev.kc_panic || page.control.live != s_dev.kc_live ||
             outputs_crc(&page) != s_dev.outputs_crc ||
             (installer() && (page.fw.check != s_dev.fw_check ||
                              page.fw.major != s_dev.fw_major ||
                              page.fw.minor != s_dev.fw_minor)))) {
            ui_page_rebuild();
            return;
        }
        if (s_dev.setup != NULL && (page.dev.state == UI_ZW_DEV_SETTING_UP ||
                                    page.dev.state == UI_ZW_DEV_WAKE_NEEDED)) {
            char text[192];
            setting_up_words(&page, text, sizeof(text));
            if (strcmp(lv_label_get_text(s_dev.setup), text) != 0) {
                lv_label_set_text(s_dev.setup, text);
            }
        }
    }
    /* A ping's answer: said once, then cleared. A "reset, broken or gone" removal's: kept on the
     * page (screens.md §6); removed, the page has nothing left to show. */
    ui_zw_progress_t p;
    read_progress(&p);
    const bool mine = p.node == s_dev.node && p.generation == s_dev.generation;
    const bool cleaning = mine && p.op == UI_ZW_OP_REMOVE_FAILED && p.step != UI_ZW_STEP_DONE &&
                          p.step != UI_ZW_STEP_IDLE;
    /* its software update: its words in place while it runs; its end kept on the page */
    const bool updating = mine && p.op == UI_ZW_OP_FIRMWARE && p.step != UI_ZW_STEP_DONE &&
                          p.step != UI_ZW_STEP_IDLE;
    if (installer() && updating != s_dev.fw_running) {
        if (!updating && p.op == UI_ZW_OP_FIRMWARE && p.step == UI_ZW_STEP_DONE) {
            s_fwres.node = s_dev.node;
            s_fwres.generation = s_dev.generation;
            s_fwres.result = p.result;
            s_fwres.why = p.fw_why;
            s_fwres.major = s_dev.fw_major;
            s_fwres.minor = s_dev.fw_minor;
            ui_sheet_close(); /* a "Stop the update?" asked as it ended is moot (UX-033) */
            ui_zw_request_t dismiss = {0};
            dismiss.kind = UI_ZW_REQ_DISMISS;
            request(&dismiss);
        }
        ui_page_rebuild();
        return;
    }
    if (updating && s_dev.fw_line != NULL) {
        ui_zw_device_page_t page;
        if (S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
            char text[160];
            fw_progress_words(&p, &page, text, sizeof(text));
            if (strcmp(lv_label_get_text(s_dev.fw_line), text) != 0) {
                lv_label_set_text(s_dev.fw_line, text);
            }
        }
    }
    if (cleaning != s_dev.cleaning) {
        s_dev.cleaning = cleaning;
        if (cleaning || p.op != UI_ZW_OP_REMOVE_FAILED || p.step != UI_ZW_STEP_DONE) {
            ui_page_rebuild();
            return;
        }
    }
    if (p.step == UI_ZW_STEP_DONE && mine) {
        if (p.op == UI_ZW_OP_ROUTES) {
            ui_toast(p.routes_stopped ? TR("Stopped: its routes stay as they were")
                     : p.result == UI_ZW_RES_DONE
                         ? TR("Its routes are rebuilt")
                         : TR("Its routes couldn't be rebuilt: it didn't answer"));
            ui_zw_request_t dismiss = {0};
            dismiss.kind = UI_ZW_REQ_DISMISS;
            request(&dismiss);
        } else if (p.op == UI_ZW_OP_IDENTIFY) {
            ui_toast(p.result == UI_ZW_RES_ANSWERED ? TR("Its light blinks 3 times")
                                                    : TR("It didn't answer: nothing blinks"));
            ui_zw_request_t dismiss = {0};
            dismiss.kind = UI_ZW_REQ_DISMISS;
            request(&dismiss);
        } else if (p.op == UI_ZW_OP_PING) {
            ui_toast(p.result == UI_ZW_RES_ANSWERED ? TR("It answered") : TR("No answer"));
            ui_zw_request_t dismiss = {0};
            dismiss.kind = UI_ZW_REQ_DISMISS;
            request(&dismiss);
        } else if (p.op == UI_ZW_OP_REMOVE_FAILED) {
            ui_zw_request_t dismiss = {0};
            dismiss.kind = UI_ZW_REQ_DISMISS;
            request(&dismiss);
            if (p.result == UI_ZW_RES_REMOVED) {
                s_clean.result = UI_ZW_RES_NONE;
                if (p.prov_forgot) {
                    ui_toast(TR("Its SmartStart code was forgotten too"));
                }
                ui_page_back(); /* the list, without it */
                return;
            }
            s_clean.node = s_dev.node;
            s_clean.generation = s_dev.generation;
            s_clean.result = p.result;
            ui_page_rebuild();
            scroll_page_top(); /* how it ended is at the top */
            return;
        }
    }
    if (s_dev.last_heard == NULL) {
        return;
    }
    ui_zw_device_page_t page;
    if (!S.be.zw_device(S.be.ctx, s_dev.node, s_dev.generation, &page)) {
        ui_page_back(); /* it left the network while its page was open */
        return;
    }
    char heard[40];
    if (page.dev.heard_s == UI_ZW_NEVER) {
        /* TR: a row's value on a device's page. At most 39 bytes */
        i18n_utf8_copy(heard, sizeof(heard), TR("Not heard yet"));
    } else {
        char span[24];
        span_words(page.dev.heard_s, span, sizeof(span));
        /* TR: a row's value on a device's page; {0} is how long, like "45 min" or "2 h 30 min".
         * At most 33 bytes */
        I18N_FORMAT(heard, sizeof(heard), TR("{0} ago"), span);
    }
    ui_set_label(s_dev.last_heard, "value_label", heard);
    /* ZW-040: the row appears with the first measured frame, and then follows it */
    if (s_dev.signal != NULL && page.dev.signal != UI_ZW_SIGNAL_UNKNOWN) {
        ui_set_label(s_dev.signal, "value_label", signal_words(page.dev.signal));
    } else if (s_dev.signal == NULL && page.dev.signal != UI_ZW_SIGNAL_UNKNOWN) {
        ui_page_rebuild();
        return;
    }
    /* a chosen check-in: "Changes to ..." until the device took it (then its new interval), or
     * gone when it did not take it (the interval it kept) */
    device_check_in_words(&page);
}

/* ---- §7: removing (PAGE_ZWAVE_REMOVE, arg 0 or a known device's node | generation << 16) ------ */

static struct {
    int32_t arg;
    zw_step_family_t family;
    bool open;
    uint32_t polled;
    bool smart;          /* the removed device was registered: its code went with it... */
    bool smart_kept;     /* ...unless the installer kept it */
    uint8_t smart_dsk[16];
} s_rm;

/* "Remove a Z-Wave device", or "Remove Front door" when arg names a device still known
 * (screens.md §7, "opened from a device page"). */
static void remove_named_title(int32_t arg, char *title, size_t tlen)
{
    uint16_t node, generation;
    ui_zw_device_page_t page;
    if (arg != 0 && S.be.zw_device != NULL) {
        device_args(arg, &node, &generation);
        if (S.be.zw_device(S.be.ctx, node, generation, &page)) {
            char name[64];
            device_name(&page.dev, name, sizeof(name));
            /* TR: a page's title; {0} is the device's name */
            I18N_FORMAT(title, tlen, TR("Remove {0}"), name);
            return;
        }
    }
    /* TR: a page's title */
    i18n_utf8_copy(title, tlen, TR("Remove a Z-Wave device"));
}

static void remove_again(void *ctx)
{
    (void)ctx;
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_REMOVE;
    req.user = S.auth_user;
    request(&req);
    ui_page_rebuild();
}

static void remove_stop(void *ctx)
{
    (void)ctx;
    ui_page_back();
}

/* "Done" after a removal that succeeded: if this page was opened from that very device's own
 * page ("Remove this device") and it really is the one that left (exclusion cannot pick, so a
 * different device may have answered instead), that page no longer has anything to show - skip
 * back past it too, to the Z-Wave page's list, rather than land on "no longer in the network". */
static void remove_done(void *ctx)
{
    (void)ctx;
    bool matched = false;
    if (s_rm.arg != 0) {
        ui_zw_progress_t p; /* read before the pop below: leaving sends DISMISS, zeroing it */
        read_progress(&p);
        uint16_t target_node = 0, target_gen = 0;
        device_args(s_rm.arg, &target_node, &target_gen);
        matched = target_node == p.node && target_gen == p.generation;
    }
    ui_page_back();
    if (matched) {
        ui_page_back();
    }
}

static void remove_add_now(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZWAVE_ADD, 0);
}

/* From a known device's removal that didn't work: its page, then its "reset, broken or gone"
 * confirmation there (the page shows the check and its outcome). */
static void remove_clean_instead_go(void)
{
    ui_page_back(); /* its page, which follows what happens next */
    remove_failed_go();
}

static void remove_clean_instead(void *ctx)
{
    (void)ctx;
    s_confirm_arg = (void *)(intptr_t)s_rm.arg;
    ui_confirm(TR("Remove it: it's reset, broken or gone?"),
               TR("The radio checks it no longer answers first. One that answers is kept: "
                  "remove it the normal way."),
               TR("Remove it"), remove_clean_instead_go);
}

static bool smart_last_dsk(uint8_t out[16]);

/* Keep the removed device's SmartStart code (it joins again once reset), or forget it again. */
static void remove_smart_toggled(bool on, void *ctx)
{
    (void)ctx;
    if (!on && !s_rm.smart_kept) {
        if (S.be.zw_prov_change(S.be.ctx, NULL, UI_ZW_PROV_KEEP, 0u) == UI_ZW_PROV_UPDATED) {
            s_rm.smart_kept = true;
            (void)smart_last_dsk(s_rm.smart_dsk); /* it was put back last */
        }
    } else if (on && s_rm.smart_kept) {
        if (S.be.zw_prov_change(S.be.ctx, s_rm.smart_dsk, UI_ZW_PROV_FORGET, 0u) ==
            UI_ZW_PROV_UPDATED) {
            s_rm.smart_kept = false;
        }
    }
}

static void render_remove_listen(const ui_zw_progress_t *p, lv_obj_t *body, char *note, size_t nlen)
{
    s_rm.smart = false; /* a new removal: nothing forgotten yet */
    s_rm.smart_kept = false;
    if (p->step == UI_ZW_STEP_LISTENING) {
        /* TR: the line under a page's title; {0} is the seconds left, 0 to 120 */
        I18N_FORMAT(note, nlen,
                    TRN("Listening · {0} s left", "Listening · {0} s left",
                        (unsigned)p->seconds_left),
                    (unsigned)p->seconds_left);
    } else {
        /* TR: the line under a page's title while the radio starts to listen */
        i18n_utf8_copy(note, nlen, TR("Starting"));
    }
    ui_paragraph(body,
                TR("Put the device in pairing mode, the same way as adding it. This also resets a "
                   "sensor that belonged to another system, so it can be added here."),
                ui_tone_gray());
    ui_row_action(body, UI_ICON_CIRCLE_X, TR_C("row button", "Stop"), ui_tone_gray(), remove_stop,
                  NULL);
}

static void render_remove_done(const ui_zw_progress_t *p, lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: the line under a page's title when the removal is over */
    i18n_utf8_copy(note, nlen, TR_C("page note", "Done"));
    switch (p->result) {
    case UI_ZW_RES_REMOVED: {
        uint16_t target_node = 0, target_gen = 0;
        if (s_rm.arg != 0) {
            device_args(s_rm.arg, &target_node, &target_gen);
        }
        const char *model = p->model[0] != '\0' ? p->model : TR("Z-Wave device");
        char text[160];
        if (s_rm.arg != 0 && (target_node != p->node || target_gen != p->generation)) {
            /* TR: a paragraph on the Remove page; {0} is the removed device's model. At most
             * 130 bytes */
            I18N_FORMAT(text, sizeof(text), TR("That was another device: {0} was removed"), model);
            ui_paragraph(body, text, ui_tone_amber());
        } else {
            /* TR: a paragraph on the Remove page; {0} is the removed device's model. At most
             * 150 bytes */
            I18N_FORMAT(text, sizeof(text), TR("Removed: {0}"), model);
            ui_paragraph(body, text, ui_tone_green());
        }
        for (uint8_t i = 0; i < p->zone_count && i < UI_ZW_ZONES_MAX; i++) {
            char zt[64];
            /* TR: a paragraph on the Remove page; {0} is a zone's number. At most 55 bytes */
            I18N_FORMAT(zt, sizeof(zt), TR("Zone {0} now needs a new sensor: it shows CHECK"),
                        (unsigned)p->zones[i]);
            ui_paragraph(body, zt, ui_tone_amber());
        }
        if (p->prov_forgot) {
            s_rm.smart = true;
        }
        if (s_rm.smart && S.be.zw_prov_change != NULL) {
            ui_row_toggle(body, TR("Forget its QR code too"),
                          TR("Else it joins again once it is reset"),
                          !s_rm.smart_kept, remove_smart_toggled, NULL);
        }
        ui_row_action(body, UI_ICON_REFRESH, TR("Remove another"), ui_tone_blue(), remove_again,
                      NULL);
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Done"), ui_tone_gray(), remove_done, NULL);
        break;
    }
    case UI_ZW_RES_REMOVED_FOREIGN:
        ui_paragraph(body, TR("Removed a device from another system: it can be added now."),
                    ui_tone_green());
        ui_row_action(body, UI_ICON_PLUS, TR("Add it now"), ui_tone_blue(), remove_add_now, NULL);
        break;
    default: /* each way it ended without removing anything says so (screens.md §7) */
        switch (p->result) {
        case UI_ZW_RES_STOPPED:
            ui_paragraph(body, TR("Stopped: nothing was removed."), ui_tone_gray());
            break;
        case UI_ZW_RES_NOTHING_REMOVED:
            ui_paragraph(body,
                         TR("No device answered in time: nothing was removed. Press its button "
                            "while the countdown runs."),
                         ui_tone_gray());
            break;
        case UI_ZW_RES_RADIO_RESTARTED: /* ZW-005 */
            ui_paragraph(body, TR("The Z-Wave radio restarted, so it stopped listening. Try again."),
                         ui_tone_amber());
            break;
        case UI_ZW_RES_TIMED_OUT:
            ui_paragraph(body,
                         TR("A device answered but didn't finish leaving: it may still be in "
                            "the network. Try again."),
                         ui_tone_amber());
            break;
        default:
            ui_paragraph(body, TR("The Z-Wave radio couldn't remove it: nothing was removed."),
                         ui_tone_amber());
            break;
        }
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), remove_again, NULL);
        if (s_rm.arg != 0) {
            /* a device that was reset, has no power or is broken can't answer this: its own way */
            ui_paragraph(body,
                         TR("If it was reset or no longer works, it can't answer this. Remove "
                            "it the other way:"),
                         ui_tone_gray());
            ui_row_link(body, UI_ICON_CIRCLE_X, ui_tone_red(),
                        TR("Remove it: it's reset, broken or gone"),
                        TR("The radio checks it no longer answers first"), NULL,
                        UI_ICON_CHEVRON_RIGHT, remove_clean_instead, NULL);
        }
        break;
    }
}

static void build_remove(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    s_rm.arg = arg;
    s_rm.open = false;
    remove_named_title(arg, title, tlen);
    ui_zw_status_t st;
    if (!read_status(&st)) {
        unavailable_paragraph(body);
        return;
    }
    if (!installer()) {
        locked_paragraph(body, LOCKED_REMOVE);
        return;
    }
    ui_zw_progress_t p;
    read_progress(&p);
    /* A removal still running is followed; a finished one only when this visit started it (a
     * rebuild of this page) - an earlier visit's result, or one the console left, is not this
     * removal: a new one starts (screens.md §7). */
    const bool running = p.op == UI_ZW_OP_REMOVE && p.step != UI_ZW_STEP_DONE &&
                         p.step != UI_ZW_STEP_IDLE;
    const bool this_visit = p.op == UI_ZW_OP_REMOVE && s_owner == PAGE_ZWAVE_REMOVE;
    if (!running && !this_visit && !S.page_fresh) {
        s_owner = 0; /* returned to: never listening again by itself (UX-005) */
        ui_paragraph(body, TR("Nothing is being removed now."), ui_tone_gray());
        ui_row_action(body, UI_ICON_TRASH, TR("Remove a device"), ui_tone_blue(), remove_again,
                      NULL);
        return;
    }
    if (!running && !this_visit) {
        ui_zw_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = UI_ZW_REQ_REMOVE;
        req.user = S.auth_user;
        const ui_zw_req_result_t r =
            S.be.zw_request != NULL ? S.be.zw_request(S.be.ctx, &req) : UI_ZW_REQ_NO_RADIO;
        if (r != UI_ZW_REQ_OK) {
            s_owner = 0;
            refused_paragraph(body, r, false);
            ui_row_action(body, UI_ICON_TRASH, TR("Try again"), ui_tone_blue(), remove_again,
                          NULL);
            return;
        }
        read_progress(&p);
    }
    s_owner = PAGE_ZWAVE_REMOVE;
    s_rm.family = step_family(&p);
    if (s_rm.family == ZWF_DONE) {
        render_remove_done(&p, body, note, nlen);
    } else {
        render_remove_listen(&p, body, note, nlen);
    }
    s_rm.open = true;
    s_rm.polled = lv_tick_get();
}

static void remove_tick(void)
{
    if (!s_rm.open || lv_tick_elaps(s_rm.polled) < POLL_MS) {
        return;
    }
    s_rm.polled = lv_tick_get();
    ui_zw_progress_t p;
    read_progress(&p);
    const zw_step_family_t fam = step_family(&p);
    if (fam != s_rm.family) {
        ui_page_rebuild();
        return;
    }
    if (fam == ZWF_LISTEN) {
        char note[64];
        if (p.step == UI_ZW_STEP_LISTENING) {
            /* TR: the line under a page's title; {0} is the seconds left, 0 to 120. At most 50
             * bytes */
            I18N_FORMAT(note, sizeof(note),
                        TRN("Listening · {0} s left", "Listening · {0} s left",
                            (unsigned)p.seconds_left),
                        (unsigned)p.seconds_left);
        } else {
            /* TR: the line under a page's title while the radio starts to listen. At most 63
             * bytes */
            i18n_utf8_copy(note, sizeof(note), TR("Starting"));
        }
        ui_page_note(note);
    }
}

/* ---- §8: region ---------------------------------------------------------------------------- */

static uint8_t s_region_picked;

/* The installer code once more, after the confirmation of something that drops devices (the
 * network reset; a region change that stops some, ZW-045): true when it is the installer's,
 * else the pad says so and stays open. */
static bool installer_code_ok(const char *code)
{
    uint8_t user = 0;
    if (S.be.authenticate == NULL || S.be.authenticate(S.be.ctx, code, &user) != ALARM_OK ||
        user != ALARM_USER_INSTALLER) {
        ui_pad_error(TR("Wrong code, try again"));
        return false;
    }
    ui_pad_cancel();
    return true;
}

static void region_confirmed(void)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_REGION;
    req.user = S.auth_user;
    req.region = s_region_picked;
    if (request(&req)) {
        ui_page_back();
    }
}

static bool region_code_ok(const char *code)
{
    if (!installer_code_ok(code)) {
        return false;
    }
    region_confirmed();
    return true;
}

/* ZW-045: a change that stops devices asks for the installer code, as the network reset does. */
static void region_ask_code(void)
{
    /* TR: a code pad's title, after confirming a Z-Wave region change that stops devices */
    ui_pad_ask(HOST_SETTINGS, UI_ICON_TRIANGLE_ALERT, TR("Confirm the region"),
               TR("Enter the installer code once more"), ui_tone_red(), region_code_ok);
}

/* What choosing `code` does (ZW-045): its ui_zw_region_change_t, and how many devices stop. */
static uint8_t region_effect(const ui_zw_status_t *st, uint8_t code, uint16_t *stop)
{
    const size_t n = st->region_count < UI_ZW_REGIONS_MAX ? st->region_count : UI_ZW_REGIONS_MAX;
    for (size_t i = 0; i < n; i++) {
        if (st->regions[i] == code) {
            *stop = st->region_stop[i];
            return st->region_change[i];
        }
    }
    *stop = st->devices; /* not one of the radio's: say the worst */
    return UI_ZW_REGION_BAND_CHANGE;
}

static void region_row_tap(void *ctx)
{
    s_region_picked = (uint8_t)(uintptr_t)ctx;
    ui_zw_status_t st;
    if (!read_status(&st) || !installer()) {
        return;
    }
    uint16_t stop = 0u;
    const uint8_t effect = region_effect(&st, s_region_picked, &stop);
    char title[64];
    /* TR: a confirmation's title; {0} is a region like "Europe" or "United States". At most 63
     * bytes */
    I18N_FORMAT(title, sizeof(title), TR("Change the region to {0}?"),
                region_name(s_region_picked));
    char text[128];
    if (stop > 0u && effect == UI_ZW_REGION_DROPS_LR) {
        /* TR: a confirmation's text: leaving a Long Range region for its classic one; {0} is the
         * number of Long Range devices, 1 or more. At most 120 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("{0} Long Range device stops working. It must be added again, and its "
                        "zones show CHECK until then.",
                        "{0} Long Range devices stop working. Each must be added again, and "
                        "their zones show CHECK until then.",
                        (unsigned)stop),
                    (unsigned)stop);
    } else if (stop > 0u) {
        /* TR: a confirmation's text: a region on another band; {0} is the number of devices, 1
         * or more. At most 120 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("{0} device stops working. It must be added again, and its zones show "
                        "CHECK until then.",
                        "All {0} devices stop working. Each must be added again, and their "
                        "zones show CHECK until then.",
                        (unsigned)stop),
                    (unsigned)stop);
    } else if (st.devices > 0u) {
        /* TR: a confirmation's text: the same band; {0} is the number of devices, 1 or more. At
         * most 120 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("The {0} device keeps working. The Z-Wave radio restarts (a few "
                        "seconds).",
                        "All {0} devices keep working. The Z-Wave radio restarts (a few "
                        "seconds).",
                        (unsigned)st.devices),
                    (unsigned)st.devices);
    } else {
        i18n_utf8_copy(text, sizeof(text),
                       TR("Changing it restarts the Z-Wave radio (a few seconds)."));
    }
    ui_confirm(title, text, TR("Change it"), stop > 0u ? region_ask_code : region_confirmed);
}

static void build_region(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                         size_t nlen)
{
    (void)arg;
    /* TR: a page's title */
    i18n_utf8_copy(title, tlen, TR("Region"));
    /* TR: the line under a page's title */
    i18n_utf8_copy(note, nlen, TR("Must match where the radio was bought"));
    ui_zw_status_t st;
    if (!read_status(&st)) {
        unavailable_paragraph(body);
        return;
    }
    if (!installer()) {
        locked_paragraph(body, LOCKED_REGION);
        return;
    }
    const size_t n = st.region_count < UI_ZW_REGIONS_MAX ? st.region_count : UI_ZW_REGIONS_MAX;
    bool stops = false;
    for (size_t i = 0; i < n; i++) {
        const uint8_t code = st.regions[i];
        const bool sel = st.region_known && code == st.region;
        /* ZW-045: what choosing it does to the devices, on its row */
        char detail[64] = "";
        if (!sel && st.region_stop[i] > 0u) {
            stops = true;
            /* TR: under a Z-Wave region on the region page: choosing it stops this many devices;
             * {0} is a number, 1 or more. At most 47 bytes */
            I18N_FORMAT(detail, sizeof(detail),
                        TRN("{0} device stops working", "{0} devices stop working",
                            (unsigned)st.region_stop[i]),
                        (unsigned)st.region_stop[i]);
        } else if (!sel && st.devices > 0u) {
            /* TR: under a Z-Wave region on the region page: choosing it keeps every device
             * working. At most 47 bytes */
            i18n_utf8_copy(detail, sizeof(detail), TR("Devices keep working"));
        }
        ui_row_link(body, UI_ICON_GLOBE, sel ? ui_tone_green() : ui_tone_gray(), region_name(code),
                   detail, sel ? TR("Selected") : NULL, sel ? NULL : UI_ICON_CHEVRON_RIGHT,
                   sel ? NULL : region_row_tap, (void *)(uintptr_t)code);
    }
    if (stops) {
        /* TR: a paragraph on the Z-Wave region page, under regions that stop devices (ZW-045) */
        ui_paragraph(body,
                    TR("Devices that stop working must be added again. Until then their zones "
                       "show CHECK."),
                    ui_tone_amber());
    }
    ui_paragraph(body, TR("Changing it restarts the Z-Wave radio (a few seconds)."),
                 ui_tone_gray());
}

/* ---- §8: radio details --------------------------------------------------------------------- */

static struct {
    lv_obj_t *state; /* the State row: its words follow the radio in place, once a second */
    lv_obj_t *noise; /* the Background noise row: the same */
    bool open;
    uint32_t polled;
    lv_obj_t *fw_line; /* §14: the update's progress, in place */
    bool fw_running;
    bool fw_asking;    /* its check or download was out when the page was drawn */
} s_radio;

static void radio_noise_words(const ui_zw_status_t *st, char *out, size_t len)
{
    if (st->noise_known) {
        /* TR: the radio's background noise in dBm, a negative number; the quietest channel's.
         * At most 47 bytes */
        I18N_FORMAT(out, len, TR("{0} dBm (quietest channel)"), (int)st->noise_dbm);
    } else {
        /* TR: a value on the radio's page before it was measured. At most 31 bytes */
        i18n_utf8_copy(out, len, TR("Not measured yet"));
    }
}

static void radio_state_words(void)
{
    ui_zw_status_t st;
    if (s_radio.state != NULL && read_status(&st)) {
        ui_set_label(s_radio.state, "value_label", radio_state_word(st.radio));
        if (s_radio.noise != NULL) {
            char text[48];
            radio_noise_words(&st, text, sizeof(text));
            ui_set_label(s_radio.noise, "value_label", text);
        }
    }
    s_radio.polled = lv_tick_get();
}

/* ---- network.md §14: the radio's own software, checked and installed only when asked ---- */

static void radio_fw_request(uint8_t kind)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    req.user = S.auth_user;
    req.node = 0u; /* the radio itself */
    if (request(&req)) {
        ui_page_rebuild();
    }
}

static void radio_fw_check(void *ctx)
{
    (void)ctx;
    radio_fw_request(UI_ZW_REQ_FW_CHECK);
}

static void radio_fw_install_go(void)
{
    radio_fw_request(UI_ZW_REQ_FW_INSTALL);
}

static void radio_fw_install(void *ctx)
{
    (void)ctx;
    ui_zw_status_t st;
    if (!installer() || !read_status(&st) || st.radio_fw_check != UI_ZW_FW_AVAILABLE) {
        return;
    }
    static char title[56];
    /* TR: a confirmation's title; {0} is a version like 7.19. At most 40 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Update the radio to {0}?"), st.radio_fw_version);
    ui_confirm(title,
               TR("Z-Wave sensors aren't heard for up to 15 minutes, and arming waits until the "
                  "radio is back. Keep the panel powered."),
               TR("Update"), radio_fw_install_go);
}

/* Why the radio's update ended without it, in words (its bootloader's code when it gave one). */
static void radio_fw_failure(const ui_zw_progress_t *p, char *out, size_t len)
{
    switch (p->fw_why) {
    case UI_ZW_FWF_REFUSED:
        if (p->fw_error == 0x18u) {
            /* TR: a paragraph on the Z-Wave radio's page. At most 159 bytes */
            i18n_utf8_copy(out, len, TR("Its bootloader refused the file (error 0x18): from 1.50 "
                                        "or 2.00, install 2.20 first."));
        } else {
            char code[8];
            snprintf(code, sizeof(code), "0x%02X", (unsigned)p->fw_error);
            /* TR: a paragraph on the Z-Wave radio's page; {0} is an error code like 0x1F. At
             * most 150 bytes */
            I18N_FORMAT(out, len, TR("Its bootloader refused the file (error {0}). It keeps its "
                                     "old software."),
                        code);
        }
        return;
    case UI_ZW_FWF_BAD_TRANSFER:
        /* TR: a paragraph on the Z-Wave radio's page. At most 159 bytes */
        i18n_utf8_copy(out, len, TR("The file kept arriving damaged. Check the radio's wires, "
                                    "then try again."));
        return;
    case UI_ZW_FWF_NO_ANSWER:
        /* TR: a paragraph on the Z-Wave radio's page. At most 159 bytes */
        i18n_utf8_copy(out, len, TR("The radio stopped answering during the update. If it isn't "
                                    "back in a few minutes, restart the panel, then try again."));
        return;
    case UI_ZW_FWF_NOT_UPGRADABLE:
        /* TR: a paragraph on the Z-Wave radio's page. At most 159 bytes */
        i18n_utf8_copy(out, len, TR("Its bootloader doesn't take updates over the wire."));
        return;
    default:
        /* TR: a paragraph on the Z-Wave radio's page. At most 159 bytes */
        i18n_utf8_copy(out, len, TR("The new software didn't start: the radio waits in its "
                                    "bootloader. Try the update again."));
        return;
    }
}

static void radio_fw_line(const ui_zw_progress_t *p, char *out, size_t len)
{
    const unsigned pct = p->fw_total != 0u ? (unsigned)((uint32_t)p->fw_done * 100u / p->fw_total)
                                           : 0u;
    /* TR: a paragraph on the Z-Wave radio's page while its update runs; {0} is 0 to 100. At
     * most 110 bytes */
    I18N_FORMAT(out, len,
                TR("Updating the radio: {0} %. Z-Wave sensors aren't heard until it's back."), pct);
}

/* ---- the radio's software from the SD card (network.md §14; the "Z-Wave updates" follow-up) ---- */

static uint8_t s_sd_shown = UI_ZW_SD_IDLE; /* the state the page shows (its tick follows it) */

static const char *sd_words(uint8_t st)
{
    switch ((ui_zw_sd_t)st) {
    /* TR: a paragraph on the Z-Wave radio's page while its file is read from the SD card */
    case UI_ZW_SD_READING: return TR("Reading the file from the SD card...");
    /* TR: a paragraph on the Z-Wave radio's page */
    case UI_ZW_SD_NO_CARD: return TR("No SD card can be read. Put the card in, then try again.");
    /* TR: a paragraph on the Z-Wave radio's page. At most 159 bytes */
    case UI_ZW_SD_NO_FILE: return TR("No .gbl file on the SD card. Copy the radio's file from its "
                                     "maker to the card's top folder, or to a folder named zwave.");
    /* TR: a paragraph on the Z-Wave radio's page */
    case UI_ZW_SD_SEVERAL: return TR("More than one .gbl file on the SD card: leave only the one to install.");
    /* TR: a paragraph on the Z-Wave radio's page */
    case UI_ZW_SD_TOO_BIG: return TR("The .gbl file on the SD card isn't a radio's file.");
    /* TR: a paragraph on the Z-Wave radio's page */
    case UI_ZW_SD_REFUSED: return TR("The file isn't one for this radio. It keeps its software.");
    /* TR: a paragraph on the Z-Wave radio's page */
    case UI_ZW_SD_NOT_ALLOWED: return TR("Disarm every partition, in installer mode, then try again.");
    /* TR: a paragraph on the Z-Wave radio's page */
    case UI_ZW_SD_BUSY: return TR("The radio is busy: try again when it's done.");
    default: return NULL;
    }
}

static void radio_sd_go(void)
{
    if (!installer() || S.be.zw_radio_sd_install == NULL) {
        return;
    }
    s_sd_shown = S.be.zw_radio_sd_install(S.be.ctx, S.auth_user);
    ui_page_rebuild();
}

static void radio_sd_install(void *ctx)
{
    (void)ctx;
    if (!installer()) {
        return;
    }
    /* TR: a confirmation's title. At most 40 bytes */
    const char *title = TR("Update the radio from the SD card?");
    ui_confirm(title,
               TR("Z-Wave sensors aren't heard for up to 15 minutes, and arming waits until the "
                  "radio is back. Keep the panel powered."),
               TR("Update"), radio_sd_go);
}

/* The page's words follow the card's reading, once a second (radio_tick). */
static void radio_sd_tick(void)
{
    if (S.be.zw_radio_sd_state == NULL || s_sd_shown == UI_ZW_SD_IDLE) {
        return;
    }
    const uint8_t now = S.be.zw_radio_sd_state(S.be.ctx);
    if (now != s_sd_shown) {
        s_sd_shown = now;
        ui_page_rebuild();
    }
}

static void radio_sd_rows(lv_obj_t *body, const ui_zw_status_t *st)
{
    if (S.be.zw_radio_sd_install == NULL || !installer()) {
        return;
    }
    const char *w = sd_words(s_sd_shown);
    if (w != NULL && s_sd_shown != UI_ZW_SD_STARTED) {
        ui_paragraph(body, w, s_sd_shown == UI_ZW_SD_READING ? ui_tone_blue() : ui_tone_amber());
    }
    if (s_sd_shown == UI_ZW_SD_READING) {
        return;
    }
    /* TR: a row on the Z-Wave radio's page */
    ui_row_link(body, UI_ICON_HARD_DRIVE, ui_tone_blue(), TR("Install from an SD card"),
                st->update_advised
                    /* TR: under "Install from an SD card" when the radio's software is old */
                    ? TR("Its maker's .gbl file for this radio, copied to the card")
                    /* TR: under "Install from an SD card" */
                    : TR("A .gbl file from the radio's maker, copied to the card"),
                NULL, UI_ICON_CHEVRON_RIGHT, radio_sd_install, NULL);
}

static void radio_software_rows(lv_obj_t *body, const ui_zw_status_t *st)
{
    ui_zw_progress_t p;
    read_progress(&p);
    s_radio.fw_line = NULL;
    s_radio.fw_asking = st->radio_fw_check == UI_ZW_FW_CHECKING ||
                        st->radio_fw_check == UI_ZW_FW_DOWNLOADING;
    s_radio.fw_running = p.op == UI_ZW_OP_RADIO_UPDATE && p.step != UI_ZW_STEP_DONE &&
                         p.step != UI_ZW_STEP_IDLE;
    ui_section(body, TR("Software"));
    if (st->update_advised && !s_radio.fw_running) {
        /* TR: a paragraph on the Z-Wave radio's page when its software is older than its maker
         * advises (Zooz 2.20). At most 190 bytes */
        ui_paragraph(body, TR("Its software is older than its maker advises. Tap Check for an "
                              "update below to install the newer one."),
                     ui_tone_amber());
    }
    if (s_radio.fw_running) {
        char text[120];
        radio_fw_line(&p, text, sizeof(text));
        s_radio.fw_line = ui_paragraph(body, text, ui_tone_blue());
        return;
    }
    if (p.op == UI_ZW_OP_RADIO_UPDATE && p.step == UI_ZW_STEP_DONE) {
        char text[160];
        if (p.result == UI_ZW_RES_FW_UPDATED) {
            /* TR: a paragraph on the Z-Wave radio's page; {0}.{1} is a version. At most 150
             * bytes */
            I18N_FORMAT(text, sizeof(text), TR("Updated: it runs {0}.{1} now."),
                        (unsigned)st->firmware_major, (unsigned)st->firmware_minor);
            ui_paragraph(body, text, ui_tone_green());
        } else {
            radio_fw_failure(&p, text, sizeof(text));
            ui_paragraph(body, text, ui_tone_amber());
        }
    }
    if (!installer()) {
        ui_row_link(body, UI_ICON_REFRESH, ui_tone_gray(), TR("Check for an update"),
                    TR("Done in installer mode"), NULL, NULL, NULL, NULL);
        return;
    }
    switch (st->radio_fw_check) {
    case UI_ZW_FW_CHECKING:
        ui_paragraph(body, TR("Asking the update service..."), ui_tone_blue());
        return;
    case UI_ZW_FW_DOWNLOADING:
        ui_paragraph(body, TR("Downloading its new software and checking it..."), ui_tone_blue());
        return;
    case UI_ZW_FW_AVAILABLE: {
        char name[48];
        /* TR: a row's title on the radio's page; {0} is a version like 7.19. At most 30 bytes */
        I18N_FORMAT(name, sizeof(name), TR("Install version {0}"), st->radio_fw_version);
        ui_row_link(body, UI_ICON_SPARKLES, ui_tone_blue(), name,
                    st->radio_fw_words[0] != '\0' ? st->radio_fw_words
                                                  : TR("Its changes aren't described"),
                    NULL, UI_ICON_CHEVRON_RIGHT, radio_fw_install, NULL);
        return;
    }
    case UI_ZW_FW_UP_TO_DATE: ui_row_value(body, TR("Updates"), TR("Up to date")); break;
    case UI_ZW_FW_UNKNOWN:
        ui_paragraph(body, TR("No update was found for this radio. Its maker's own file can still "
                              "be installed from an SD card or the Panel Manager."),
                     ui_tone_gray());
        break;
    case UI_ZW_FW_CHECK_FAILED:
    case UI_ZW_FW_FILE_FAILED: ui_paragraph(body, st->radio_fw_words, ui_tone_amber()); break;
    default: break;
    }
    radio_sd_rows(body, st);
    ui_row_action(body, UI_ICON_REFRESH,
                  st->radio_fw_check == UI_ZW_FW_NONE ? TR("Check for an update")
                                                      : TR("Check again"),
                  ui_tone_blue(), radio_fw_check, NULL);
    if (st->radio_fw_check == UI_ZW_FW_NONE) {
        ui_paragraph(body, TR("Checked only when you ask; nothing installs by itself."),
                     ui_tone_gray());
    }
}

/* "Restart the radio": a soft reset, the start-up again (network.md §12). The State row says how
 * it goes - Starting, then Working - so there is no toast; a refusal says why in one. */
static void restart_go(void)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_RESTART;
    req.user = S.auth_user;
    if (request(&req)) {
        radio_state_words(); /* "Starting", at once */
    }
}

static void restart_row_tap(void *ctx)
{
    (void)ctx;
    if (!installer()) {
        return;
    }
    ui_confirm(TR("Restart the Z-Wave radio?"), TR("Takes a few seconds; zones keep their state."),
               TR("Restart"), restart_go);
}

static void build_radio(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen)
{
    (void)arg;
    s_radio.open = false;
    s_radio.state = NULL;
    s_radio.noise = NULL;
    s_radio.noise = NULL;
    /* TR: a page's title */
    i18n_utf8_copy(title, tlen, TR("Z-Wave radio"));
    /* TR: the line under a page's title */
    i18n_utf8_copy(note, nlen, TR("What the panel knows about its radio"));
    ui_zw_status_t st;
    if (!read_status(&st)) {
        unavailable_paragraph(body);
        return;
    }
    s_radio.state = ui_row_value(body, TR("State"), radio_state_word(st.radio));
    char sw[48];
    /* nothing read from it yet (it isn't answering): not known, never "0.0" (B015 UX-030) */
    const bool known = st.firmware_major != 0u || st.firmware_minor != 0u || st.sdk_major != 0u;
    if (known) {
        snprintf(sw, sizeof(sw), "%u.%u · %s %u.%u.%u", (unsigned)st.firmware_major,
                 (unsigned)st.firmware_minor, NOTR("Z-Wave") /* Z-Wave term */,
                 (unsigned)st.sdk_major, (unsigned)st.sdk_minor, (unsigned)st.sdk_patch);
    } else {
        /* TR: a row's value on the radio's page. At most 47 bytes */
        i18n_utf8_copy(sw, sizeof(sw), TR("Not known yet"));
    }
    ui_row_value(body, TR("Radio software"), sw);
    ui_row_value(body, TR("Region"),
                 st.region_known ? region_name(st.region)
                 : known         ? TR_C("Z-Wave region", "Unknown")
                                 : TR("Not known yet"));
    char noise[48];
    radio_noise_words(&st, noise, sizeof(noise));
    /* TR: a row on the Z-Wave radio's page */
    s_radio.noise = ui_row_value(body, TR("Background noise"), noise);
    ui_zw_progress_t busy;
    read_progress(&busy);
    const bool updating = st.radio == UI_ZW_RADIO_UPDATING;
    const bool running = busy.op != UI_ZW_OP_NONE && busy.step != UI_ZW_STEP_DONE &&
                         busy.step != UI_ZW_STEP_IDLE;
    if (installer() && (updating || running)) {
        /* ZW-024: a restart is refused while the radio updates or another change runs */
        ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_gray(), TR("Restart the radio"),
                    updating ? TR("Not while its software installs")
                             : TR("Not while another change runs"),
                    NULL, NULL, NULL, NULL);
    } else if (installer()) {
        ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_blue(), TR("Restart the radio"),
                    TR("Takes a few seconds; zones keep their state"), NULL,
                    UI_ICON_CHEVRON_RIGHT, restart_row_tap, NULL);
    } else {
        ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_gray(), TR("Restart the radio"),
                    TR("Done in installer mode"), NULL, NULL, NULL, NULL);
    }
    if (st.update_advised) {
        ui_paragraph(body, TR("Its Z-Wave software is older than the panel wants: update it."),
                    ui_tone_amber());
    }
    radio_software_rows(body, &st);
    s_radio.open = true;
    s_radio.polled = lv_tick_get();
}

static void radio_tick(void)
{
    if (!s_radio.open || lv_tick_elaps(s_radio.polled) < POLL_MS) {
        return;
    }
    radio_state_words();
    if (s_radio.fw_running) { /* its progress in place, once a second at most; its end redraws */
        ui_zw_progress_t p;
        read_progress(&p);
        if (p.op != UI_ZW_OP_RADIO_UPDATE || p.step == UI_ZW_STEP_DONE) {
            ui_page_rebuild();
            return;
        }
        if (s_radio.fw_line != NULL) {
            char text[120];
            radio_fw_line(&p, text, sizeof(text));
            lv_label_set_text(s_radio.fw_line, text);
        }
    } else {
        radio_sd_tick(); /* the card's reading, its outcome in place */
        ui_zw_status_t st;
        if (read_status(&st) && (st.radio_fw_check == UI_ZW_FW_CHECKING ||
                                 st.radio_fw_check == UI_ZW_FW_DOWNLOADING) != s_radio.fw_asking) {
            ui_page_rebuild(); /* the service answered */
        }
    }
}

/* ---- §8: reset (the two confirmations of the Z-Wave page's own row) ------------------------- */

static void reset_go(void)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_RESET;
    req.user = S.auth_user;
    if (request(&req)) {
        ui_page_rebuild();
    }
}

/* The second, plain confirmation: the installer code once more (CONTRIBUTING.md "Rules for the
 * UI"; the same weight as leaving installer mode by a lost-code recovery). */
static bool reset_code_ok(const char *code)
{
    if (!installer_code_ok(code)) {
        return false;
    }
    reset_go();
    return true;
}

static void reset_ask_code(void)
{
    ui_pad_ask(HOST_SETTINGS, UI_ICON_TRIANGLE_ALERT, TR("Confirm the reset"),
               TR("Enter the installer code once more"), ui_tone_red(), reset_code_ok);
}

static void reset_row_tap(void *ctx)
{
    (void)ctx;
    ui_zw_status_t st;
    if (!read_status(&st) || !installer()) {
        return;
    }
    char text[128];
    if (st.devices == 0u) {
        /* TR: a confirmation's text when no device has joined. At most 120 bytes */
        i18n_utf8_copy(text, sizeof(text),
                       TR("The panel starts a new Z-Wave network. No devices have joined, so "
                          "nothing else changes."));
    } else {
        /* TR: a confirmation's text; {0} is the number of devices, 1 or more. At most 120
         * bytes */
        I18N_FORMAT(text, sizeof(text),
                    TRN("{0} device loses the panel. It must be reset and added again, and its "
                        "zones show CHECK until then.",
                        "All {0} devices lose the panel. Each one must be reset and added again, "
                        "and their zones show CHECK until then.",
                        (unsigned)st.devices),
                    (unsigned)st.devices);
    }
    ui_confirm(TR("Reset the Z-Wave network?"), text, TR("Reset the network"), reset_ask_code);
}

/* ---- §4: adding a device (PAGE_ZWAVE_ADD, arg 0, 1 or 2 - see s_add.arg below) ----------------- */

static struct {
    int32_t arg;    /* 0: normal (offers "Use it for"); 1: from the zone picker (screens.md §5);
                     * 2: a new device in a failed one's place (s_replace_arg) */
    uint8_t last_step;
    bool open;
    uint32_t polled;
} s_add;
static bool s_add_accepted; /* "Keep it (this way)" was tapped: show the plain Added page */
static lv_obj_t *s_pin_countdown;
static lv_obj_t *s_allow_countdown; /* the Allow page's "Time left" value */

/* NOTHING_JOINED stays on the listening page (screens.md §4); everything else that ends the wait
 * goes to the results page. */
static zw_step_family_t add_family(const ui_zw_progress_t *p)
{
    if (p->step == UI_ZW_STEP_DONE && p->result == UI_ZW_RES_NOTHING_JOINED) {
        return ZWF_LISTEN;
    }
    return step_family(p);
}

/* The request this page makes: add, or replace (arg 2). */
static void add_request(ui_zw_request_t *req)
{
    memset(req, 0, sizeof(*req));
    req->kind = UI_ZW_REQ_ADD;
    req->user = S.auth_user;
    if (s_add.arg == 2) {
        req->kind = UI_ZW_REQ_REPLACE;
        device_args(s_replace_arg, &req->node, &req->generation);
    }
}

static void add_again(void *ctx)
{
    (void)ctx;
    ui_zw_request_t req;
    add_request(&req);
    request(&req);
    ui_page_rebuild();
}

static void add_keep_listening(void *ctx)
{
    (void)ctx;
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_KEEP_LISTENING;
    req.user = S.auth_user;
    request(&req);
}

static void add_open_remove(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZWAVE_REMOVE, 0);
}

static void add_stop(void *ctx)
{
    (void)ctx;
    ui_page_back(); /* ui_zwave_stop() sends the STOP (screens.md: "Leaving the page stops
                     * listening") */
}

static void render_add_listen(const ui_zw_progress_t *p, lv_obj_t *body, char *note, size_t nlen)
{
    if (p->step == UI_ZW_STEP_DONE) { /* UI_ZW_RES_NOTHING_JOINED */
        i18n_utf8_copy(note, nlen, TR("Nothing joined")); /* TR: The page's subtitle. At most 159 bytes. */
        ui_paragraph(body, TR("Nothing joined. Check the device's pairing steps, then try again."),
                    ui_tone_gray());
        /* ZW-006: the commonest cause is a device that still belongs to another network */
        ui_paragraph(body,
                    TR("If it was used with another system, remove it first: put it in pairing "
                       "mode on the Remove page, then add it."),
                    ui_tone_gray());
        ui_row_action(body, UI_ICON_TRASH, TR("Remove a device"), ui_tone_blue(), add_open_remove,
                      NULL);
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        return;
    }
    if (p->step == UI_ZW_STEP_LISTENING) {
        /* TR: The page's subtitle while listening. At most 156 bytes. */
        I18N_FORMAT(note, nlen,
                    TRN("Listening · {0} s left", "Listening · {0} s left",
                        (unsigned)p->seconds_left),
                    (unsigned)p->seconds_left);
    } else {
        /* TR: The page's subtitle. At most 159 bytes. */
        i18n_utf8_copy(note, nlen,
                       p->replacing ? TR("Checking the old device is gone") : TR("Starting"));
    }
    if (p->replacing && p->step == UI_ZW_STEP_STARTING) {
        /* ZW-024 (network.md §10): the radio isn't listening while it checks the old device is
         * gone (pings, then the radio's failed list): asking for the new one's button now would
         * lose its node information frame. The words come once it listens. */
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop"), ui_tone_gray(), add_stop, NULL);
        return;
    }
    ui_paragraph(body,
                p->replacing
                    ? TR("Put the new device in pairing mode now: it takes the old one's place, and "
                         "its zones follow it. Most sensors: press their Z-Wave button 3 times.")
                    : TR("Put the device in pairing mode now. Most sensors: press their Z-Wave "
                         "button 3 times quickly. Keep it within a few metres of the panel."),
                ui_tone_gray());
    static const char *const STEPS[] = {TR_NOOP("Listening for the device"),
                                        TR_NOOP("Connecting"), TR_NOOP("Securing"),
                                        TR_NOOP("Reading what it can do")};
    for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
        ui_paragraph(body, TR(STEPS[i]), i == 0u ? ui_tone_blue() : ui_tone_gray());
    }
    if (!p->replacing) { /* the radio's replacement window is its own */
        ui_row_action(body, UI_ICON_REFRESH, TR("Keep listening"), ui_tone_blue(), add_keep_listening,
                      NULL);
    }
    ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop"), ui_tone_gray(), add_stop, NULL);
}

/* ---- §4: the secure connection question -------------------------------------------------------- */

static uint8_t s_allow_keys;

static void allow_toggle(bool on, void *ctx)
{
    const uint8_t bit = (uint8_t)(uintptr_t)ctx;
    if (on) {
        s_allow_keys |= bit;
    } else {
        s_allow_keys = (uint8_t)(s_allow_keys & (uint8_t)~bit);
    }
}

static void allow_insecure_go(void)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_INSECURE;
    req.user = S.auth_user;
    request(&req);
}

static void allow_insecure(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Add it without encryption?"),
              TR("Its signal could be copied and it can't guard fire or CO."),
              TR("Add it without encryption"), allow_insecure_go);
}

static void allow_go(void *ctx)
{
    (void)ctx;
    if (s_allow_keys == 0u) { /* "Turning every toggle off equals the red button" */
        allow_insecure(NULL);
        return;
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_ALLOW;
    req.user = S.auth_user;
    req.keys = s_allow_keys;
    request(&req);
}

static void render_add_allow(const ui_zw_progress_t *p, lv_obj_t *body, char *title, size_t tlen,
                             char *note, size_t nlen)
{
    i18n_utf8_copy(title, tlen, TR("Secure connection")); /* TR: The page title. At most 47 bytes. */
    if (p->model[0] != '\0') {
        /* TR: The page's subtitle. At most 119 bytes (the device's model is up to 39). */
        I18N_FORMAT(note, nlen, TR("{0} asks for its messages to be encrypted"), p->model);
    } else {
        /* TR: The page's subtitle. At most 159 bytes. */
        i18n_utf8_copy(note, nlen, TR("The device asks for its messages to be encrypted"));
    }
    s_allow_keys = p->requested;
    if ((p->requested & UI_ZW_KEY_S2_ACCESS) != 0u) {
        ui_row_toggle(body, TR_C("device security", "Access control"),
                     TR("For locks, keypads and garage doors"), true, allow_toggle,
                     (void *)(uintptr_t)UI_ZW_KEY_S2_ACCESS);
    }
    if ((p->requested & UI_ZW_KEY_S2_AUTH) != 0u) {
        ui_row_toggle(body, TR_C("device security", "With its PIN"),
                     TR("Needs the 5-digit PIN printed on it"), true, allow_toggle, (void *)(uintptr_t)UI_ZW_KEY_S2_AUTH);
    }
    if ((p->requested & UI_ZW_KEY_S2_UNAUTH) != 0u) {
        ui_row_toggle(body, TR_C("device security", "Without a PIN"),
                     TR("Encrypted, no PIN needed"), true, allow_toggle,
                     (void *)(uintptr_t)UI_ZW_KEY_S2_UNAUTH);
    }
    if ((p->requested & UI_ZW_KEY_S0) != 0u) {
        ui_row_toggle(body, TR_C("device security", "Older security (S0)"),
                     TR("Weaker; only if nothing else works"), true, allow_toggle, (void *)(uintptr_t)UI_ZW_KEY_S0);
    }
    /* its kind isn't known before its interview: words true of a sensor and a switch alike
     * (B015 UX-052) */
    ui_paragraph(body,
                 TR("Allow it. Without encryption its messages could be faked: a sensor can't "
                    "guard fire or CO, a switch can be worked by others."),
                 ui_tone_gray());
    /* ZW-029: a live value, like the PIN page's, not a number frozen when the page was built */
    char left[32];
    /* TR: The time left, minutes then seconds. At most 27 bytes. */
    I18N_FORMAT(left, sizeof(left), TR("{0} min {1} s"), (unsigned)(p->seconds_left / 60u),
                (unsigned)(p->seconds_left % 60u));
    s_allow_countdown = ui_row_value(body, TR("Time left"), left);
    ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Allow"), ui_tone_green(), allow_go, NULL);
    ui_row_action(body, UI_ICON_CIRCLE_X, TR("Add it without encryption"), ui_tone_red(),
                 allow_insecure, NULL);
}

/* ---- §4: the PIN ------------------------------------------------------------------------------- */

static void pin_typed(const char *text)
{
    const size_t len = strlen(text);
    if (len != 5u) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Type all 5 digits"));
        return;
    }
    uint32_t pin = 0u;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            ui_beep(UI_BEEP_ERROR);
            ui_toast(TR("Digits only"));
            return;
        }
        pin = pin * 10u + (uint32_t)(*p - '0');
    }
    if (pin > UI_ZW_PIN_MAX) { /* 5 digits, but no 16-bit key group: never cut down to one */
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("That can't be its PIN: check the label"));
        return;
    }
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_PIN;
    req.user = S.auth_user;
    req.pin = pin;
    request(&req);
}

static void pin_edit(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Enter its PIN"), TR("The first 5 digits of the key on its label or box"),
                       "", UI_TEXT_NUMBER, 5, pin_typed);
}

static void pin_cancel_go(void)
{
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_INSECURE;
    req.user = S.auth_user;
    request(&req);
}

static void pin_cancel(void *ctx)
{
    (void)ctx;
    /* ZW-005: it sends the same request as the Allow page's red row, and says so */
    ui_confirm(TR("Add it without encryption?"), TR("It stays in the network without encryption."),
               TR("Add it without encryption"), pin_cancel_go);
}

static void render_add_pin(const ui_zw_progress_t *p, lv_obj_t *body, char *title, size_t tlen,
                           char *note, size_t nlen)
{
    i18n_utf8_copy(title, tlen, TR("Enter its PIN")); /* TR: The page title. At most 47 bytes. */
    /* TR: The page's subtitle. At most 159 bytes. */
    i18n_utf8_copy(note, nlen, TR("The first 5 digits of the key on its label or box"));
    char line1[32];
    char line2[32];
    dsk_rest(p->dsk, line1, sizeof(line1), line2, sizeof(line2));
    if (line1[0] != '\0') {
        char text[80];
        /* TR: At most 48 bytes (the key's digits come after). */
        I18N_FORMAT(text, sizeof(text), TR("The rest of the key should read: {0}"), line1);
        ui_paragraph(body, text, ui_tone_gray());
    }
    if (line2[0] != '\0') {
        ui_paragraph(body, line2, ui_tone_gray());
    }
    ui_paragraph(body, TR("If it doesn't match the label, this is not your device: stop."),
                ui_tone_amber());
    char left[32];
    /* TR: The time left, minutes then seconds. At most 27 bytes. */
    I18N_FORMAT(left, sizeof(left), TR("{0} min {1} s"), (unsigned)(p->seconds_left / 60u),
                (unsigned)(p->seconds_left % 60u));
    s_pin_countdown = ui_row_value(body, TR("Time left"), left);
    ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_blue(), TR("Enter the PIN"), NULL, NULL,
               UI_ICON_CHEVRON_RIGHT, pin_edit, NULL);
    ui_row_action(body, UI_ICON_CIRCLE_X, TR("Add it without encryption"), ui_tone_gray(),
                  pin_cancel, NULL);
}

/* ---- §4: progress -------------------------------------------------------------------------------- */

static struct {
    lv_obj_t *row[4]; /* Connected, Secured, Reading what it can do, Ready to use */
    lv_obj_t *asleep_box;
    lv_obj_t *stalled_box;
    bool stalled; /* a rebuild when this flips: its "Try again" row comes with it */
    bool retrying; /* tried again on this visit: the box and row stay, their words change */
    char model[UI_ZW_MODEL_LEN];
} s_prog;

static void add_setup_again(void *ctx)
{
    (void)ctx;
    ui_zw_progress_t p;
    read_progress(&p);
    const uint32_t packed = (uint32_t)p.node | (uint32_t)p.generation << 16;
    setup_again((void *)(intptr_t)(int32_t)packed);
}

static void add_stop_finish_later(void *ctx)
{
    (void)ctx;
    ui_zw_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZW_REQ_STOP;
    req.user = S.auth_user;
    request(&req);
    ui_page_rebuild();
}

/* One of the checklist's four rows, as it stands right now (called only for steps CONNECTING,
 * SECURING and READING - the family this renders). */
static void progress_row_words(const ui_zw_progress_t *p, size_t i, const char **glyph,
                               lv_color_t *tone, char *detail, size_t dlen)
{
    switch (i) {
    case 0: /* Connected */
        if (p->step > UI_ZW_STEP_CONNECTING) {
            *glyph = UI_ICON_CIRCLE_CHECK;
            *tone = ui_tone_green();
            /* TR: A checklist row's detail. At most 44 bytes. */
            I18N_FORMAT(detail, dlen, TR("Device {0}"), (unsigned)p->node);
        } else {
            *glyph = UI_ICON_CIRCLE_DOT;
            *tone = ui_tone_gray();
            i18n_utf8_copy(detail, dlen, TR("Connecting")); /* TR: A checklist row's detail. At most 47 bytes. */
        }
        break;
    case 1: /* Secured */
        if (p->step > UI_ZW_STEP_SECURING) {
            const bool none = security_warns(p->security) || p->security == UI_ZW_SEC_UNKNOWN;
            *glyph = none ? UI_ICON_CIRCLE_ALERT : UI_ICON_CIRCLE_CHECK;
            *tone = none ? ui_tone_amber() : ui_tone_green();
            /* TR: A checklist row's detail. At most 47 bytes. */
            i18n_utf8_copy(detail, dlen,
                           p->security == UI_ZW_SEC_UNCONFIRMED ? security_full(p->security)
                           : none                               ? TR("Not encrypted")
                                                                : security_full(p->security));
        } else {
            *glyph = UI_ICON_CIRCLE_DOT;
            *tone = ui_tone_gray();
            i18n_utf8_copy(detail, dlen, TR("Securing")); /* TR: A checklist row's detail. At most 47 bytes. */
        }
        break;
    case 2: /* Reading what it can do */
        if (p->step == UI_ZW_STEP_READING && p->stalled) {
            /* ZW-019: the row says it stopped, not "Step 2 of 16" with a working dot */
            *glyph = UI_ICON_CIRCLE_ALERT;
            *tone = ui_tone_amber();
            /* TR: A checklist row's detail. At most 47 bytes. */
            i18n_utf8_copy(detail, dlen, TR("Stopped: it didn't answer"));
        } else if (p->step == UI_ZW_STEP_READING) {
            *glyph = UI_ICON_CIRCLE_DOT;
            *tone = ui_tone_blue();
            if (p->setup_total != 0u) {
                /* TR: A checklist row's detail. At most 40 bytes. */
                I18N_FORMAT(detail, dlen, TR("Step {0} of {1}"), (unsigned)p->setup_done,
                            (unsigned)p->setup_total);
            } else {
                i18n_utf8_copy(detail, dlen, TR("Reading what it can do")); /* TR: A checklist row's detail. At most 47 bytes. */
            }
        } else if (p->step > UI_ZW_STEP_READING) {
            *glyph = UI_ICON_CIRCLE_CHECK;
            *tone = ui_tone_green();
            i18n_utf8_copy(detail, dlen, TR_C("checklist state", "Done")); /* TR: A checklist row's detail. At most 47 bytes. */
        } else {
            *glyph = UI_ICON_CIRCLE_DOT;
            *tone = ui_tone_gray();
            i18n_utf8_copy(detail, dlen, TR_C("checklist state", "Waiting")); /* TR: A checklist row's detail. At most 47 bytes. */
        }
        break;
    default: /* Ready to use: not reached from this page - the results page takes over */
        *glyph = UI_ICON_CIRCLE_DOT;
        *tone = ui_tone_gray();
        detail[0] = '\0';
        break;
    }
}

/* The add page's device was tried again on this visit (its "Try again"). */
static bool retried_here(const ui_zw_progress_t *p)
{
    return p->node != 0u && s_retry.node == p->node && s_retry.generation == p->generation;
}

static void stalled_words(const ui_zw_progress_t *p, char *out, size_t len, lv_color_t *tone)
{
    if (!p->stalled && retried_here(p)) {
        I18N_FORMAT(out, len, TR("Trying its setup again: step {0} of {1}."),
                    (unsigned)p->setup_done, (unsigned)p->setup_total);
        *tone = ui_tone_blue();
    } else {
        i18n_utf8_copy(out, len,
                       retried_here(p)
                           ? TR("Tried again. It stopped answering before its setup finished. Keep "
                                "it close to the panel, check its power, then try again.")
                           : TR("It stopped answering before its setup finished. Keep it close to "
                                "the panel, check its power, then try again."));
        *tone = ui_tone_amber();
    }
}

static void render_add_progress(const ui_zw_progress_t *p, lv_obj_t *body, char *title,
                                size_t tlen, char *note, size_t nlen)
{
    if (p->model[0] != '\0') {
        /* TR: The page title. At most 8 bytes (the device's model is up to 39). */
        I18N_FORMAT(title, tlen, TR("Adding {0}"), p->model);
    } else {
        i18n_utf8_copy(title, tlen, TR("Adding the device")); /* TR: The page title. At most 47 bytes. */
    }
    snprintf(s_prog.model, sizeof(s_prog.model), "%s", p->model);
    /* TR: The page's subtitle. At most 159 bytes. */
    i18n_utf8_copy(note, nlen, TR("Keep the device close to the panel"));
    static const char *const NAMES[4] = {TR_NOOP("Connected"), TR_NOOP("Secured"),
                                         TR_NOOP("Reading what it can do"),
                                         TR_NOOP("Ready to use")};
    for (size_t i = 0; i < 4u; i++) {
        const char *glyph;
        lv_color_t tone;
        char detail[48];
        progress_row_words(p, i, &glyph, &tone, detail, sizeof(detail));
        s_prog.row[i] = ui_row_link(body, glyph, tone, TR(NAMES[i]), detail, NULL, NULL, NULL, NULL);
    }
    /* the device's own wake words when its quirk has them, else the generic ones (screens.md §4) */
    char wake_words[UI_ZW_WORDS_LEN];
    i18n_utf8_copy(wake_words, sizeof(wake_words), TR("see its manual, usually by pressing its button"));
    ui_zw_device_page_t wake_page;
    if (S.be.zw_device != NULL && S.be.zw_device(S.be.ctx, p->node, p->generation, &wake_page)) {
        wake_words_of(&wake_page, wake_words, sizeof(wake_words));
    }
    /* A sleeping device that dozed off waits for its wake-up; one that stopped answering (its
     * attempts ran out) is not asleep - a keypad woken by the panel's call, a mains device -
     * and says so (network.md §5). */
    char asleep_text[192];
    I18N_FORMAT(asleep_text, sizeof(asleep_text),
                TR("It went to sleep before it finished. Wake it: {0}."), wake_words);
    s_prog.asleep_box = ui_paragraph(body, asleep_text, ui_tone_amber());
    lv_obj_set_flag(s_prog.asleep_box, LV_OBJ_FLAG_HIDDEN, !p->asleep || p->stalled);
    s_prog.stalled = p->stalled;
    /* once it stopped, its box and "Try again" stay while it is tried again: the words change,
     * nothing jumps (B015 UX-008) */
    s_prog.retrying = retried_here(p);
    char stalled_text[192];
    lv_color_t stalled_tone;
    stalled_words(p, stalled_text, sizeof(stalled_text), &stalled_tone);
    s_prog.stalled_box = ui_paragraph(body, stalled_text, stalled_tone);
    lv_obj_set_flag(s_prog.stalled_box, LV_OBJ_FLAG_HIDDEN, !p->stalled && !s_prog.retrying);
    if (p->step == UI_ZW_STEP_READING && (p->stalled || s_prog.retrying)) {
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_setup_again, NULL);
    }
    if (p->step == UI_ZW_STEP_READING) {
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop and finish later"), ui_tone_gray(),
                     add_stop_finish_later, NULL);
    }
}

/* Every second, while the step itself hasn't changed: the checklist's words and the asleep box,
 * never the rows themselves (UI-020, UI-023). */
static void update_add_progress(const ui_zw_progress_t *p)
{
    if (strcmp(p->model, s_prog.model) != 0) {
        snprintf(s_prog.model, sizeof(s_prog.model), "%s", p->model);
        char title[64];
        if (p->model[0] != '\0') {
            /* TR: The page title. At most 24 bytes (the device's model is up to 39). */
            I18N_FORMAT(title, sizeof(title), TR("Adding {0}"), p->model);
        } else {
            i18n_utf8_copy(title, sizeof(title), TR("Adding the device")); /* TR: The page title. At most 63 bytes. */
        }
        ui_set_str(&subject_page_title, title);
    }
    for (size_t i = 0; i < 4u; i++) {
        if (s_prog.row[i] == NULL) {
            continue;
        }
        const char *glyph;
        lv_color_t tone;
        char detail[48];
        progress_row_words(p, i, &glyph, &tone, detail, sizeof(detail));
        set_chip(s_prog.row[i], glyph, tone);
        set_words(s_prog.row[i], "detail_label", detail, tone);
    }
    if (s_prog.asleep_box != NULL) {
        lv_obj_set_flag(s_prog.asleep_box, LV_OBJ_FLAG_HIDDEN, !p->asleep || p->stalled);
    }
    if (p->stalled != s_prog.stalled && !s_prog.retrying) {
        ui_page_rebuild(); /* its "Try again" row, the first time */
        return;
    }
    s_prog.stalled = p->stalled;
    if (s_prog.stalled_box != NULL && s_prog.retrying) {
        char text[192];
        lv_color_t tone;
        stalled_words(p, text, sizeof(text), &tone);
        if (strcmp(lv_label_get_text(s_prog.stalled_box), text) != 0) {
            lv_label_set_text(s_prog.stalled_box, text);
            lv_obj_set_style_text_color(s_prog.stalled_box, tone, 0);
        }
    }
}

/* ---- §4: the results ------------------------------------------------------------------------- */

static void add_not_now(void *ctx)
{
    (void)ctx;
    ui_page_back();
}

static void add_continue_to_picker(void *ctx)
{
    (void)ctx;
    ui_page_back(); /* the picker beneath refreshes with this device on top (screens.md §4, §5) */
}

/* The device's page in place of the add page (its result seen): back from it is the Z-Wave page,
 * never an add page that would start listening again. */
static void add_open_device(void *ctx)
{
    ui_page_back();
    ui_page_open(PAGE_ZWAVE_DEVICE, (int32_t)(intptr_t)ctx);
}

/* A device that just joined answers, and REMOVE_FAILED refuses one that does (ZW-016): open the
 * normal exclusion page for it instead (screens.md §7), the same as "Remove this device". */
static void add_remove_and_retry_go(void)
{
    ui_zw_progress_t p;
    read_progress(&p);
    const uint32_t packed = (uint32_t)p.node | (uint32_t)p.generation << 16;
    ui_page_open(PAGE_ZWAVE_REMOVE, (int32_t)packed);
}

static void add_remove_direct(void *ctx)
{
    (void)ctx;
    add_remove_and_retry_go();
}

static void add_remove_and_retry(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Remove it and try again?"), TR("Pair it once to remove it, then add it."),
               TR("Remove it"), add_remove_and_retry_go);
}

static void add_keep_it(void *ctx)
{
    (void)ctx;
    s_add_accepted = true;
    ui_page_rebuild();
}

/* The plain "Added" page (screens.md 08/17): a real success, whether secure or not - security
 * NONE without a failure is simply an unencrypted device (the motion sensor's own case). */
static void render_added(const ui_zw_progress_t *p, lv_obj_t *body, char *title, size_t tlen,
                         char *note, size_t nlen)
{
    const bool secure = !security_warns(p->security) && p->security != UI_ZW_SEC_UNKNOWN;
    i18n_utf8_copy(title, tlen, p->replacing ? TR("Replaced") : TR("Added")); /* TR: The page title. At most 47 bytes. */
    const char *const shown = p->model[0] != '\0' ? p->model : kind_words(UI_ZW_KIND_DEVICE);
    if (secure) { /* the page's subtitle (screens.md §4), not a body paragraph */
        snprintf(note, nlen, "%s · %s", shown, TR("Secure"));
    } else {
        i18n_utf8_copy(note, nlen, shown);
    }
    ui_zw_device_page_t page;
    const bool have =
        S.be.zw_device != NULL && S.be.zw_device(S.be.ctx, p->node, p->generation, &page);
    const bool switches = have && page.output_count > 0u && page.input_count == 0u;
    if (have && p->model[0] == '\0') { /* no model name: what it is (B015 UX-056) */
        if (secure) {
            snprintf(note, nlen, "%s · %s", kind_words(page.dev.kind), TR("Secure"));
        } else {
            i18n_utf8_copy(note, nlen, kind_words(page.dev.kind));
        }
    }
    if (!secure && switches) {
        ui_paragraph(body, TR("Not encrypted: someone nearby could fake its commands and switch it."),
                     ui_tone_amber());
    } else if (secure) {
        char ci[24] = "";
        if (have && !page.dev.listening) {
            checkin_sentence(page.check_in_s, ci, sizeof(ci));
        }
        char trust[160];
        if (ci[0] != '\0') {
            I18N_FORMAT(trust, sizeof(trust), TR("Encrypted: its signal can't be copied. Checks in {0}."),
                        ci);
        } else {
            i18n_utf8_copy(trust, sizeof(trust), TR("Encrypted: its signal can't be copied."));
        }
        ui_paragraph(body, trust, ui_tone_green());
    } else {
        ui_paragraph(body,
                    TR("Not encrypted: its signal can be copied and it can't guard fire or CO."),
                    ui_tone_amber());
    }
    if (p->replacing) {
        /* §10: its zones are the old device's - nothing to set up again */
        ui_paragraph(body, TR("It took the old device's place: its zones follow it."), ui_tone_gray());
        const uint32_t packed = (uint32_t)p->node | (uint32_t)p->generation << 16;
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Open it"), ui_tone_blue(), add_open_device,
                      (void *)(intptr_t)(int32_t)packed);
        ui_row_action(body, UI_ICON_CIRCLE_SLASH, TR("Done"), ui_tone_gray(), add_not_now, NULL);
        return;
    }
    if (s_add.arg == 1) {
        ui_paragraph(body, TR("Pick its input on the previous page."), ui_tone_gray());
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Continue"), ui_tone_blue(),
                     add_continue_to_picker, NULL);
        return;
    }
    if (have && page.dev.kind == UI_ZW_KIND_KEYPAD && page.input_count == 0u) {
        /* a keypad feeds no zone: its settings are on its page (B015 UX-009) */
        i18n_utf8_copy(title, tlen, TR("Keypad added")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                     TR("Set its sounds and lights, and whether its buttons arm and disarm, on "
                        "its page."),
                     ui_tone_gray());
        const uint32_t packed = (uint32_t)p->node | (uint32_t)p->generation << 16;
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Open it"), ui_tone_blue(), add_open_device,
                      (void *)(intptr_t)(int32_t)packed);
        ui_row_action(body, UI_ICON_CIRCLE_SLASH, TR("Done"), ui_tone_gray(), add_not_now, NULL);
        return;
    }
    if (have && page.output_count > 0u && page.input_count == 0u) {
        /* a switch, a dimmer, a relay kit: nothing to make a zone of (ZW-034) */
        /* TR: The page title. At most 47 bytes. */
        i18n_utf8_copy(title, tlen,
                       page.outputs[0].kind == UI_ZW_OUT_DIMMER ? TR("Dimmer added")
                       : page.output_count > 1u                  ? TR("Relays added")
                                                                 : TR("Switch added"));
        ui_paragraph(body,
                     page.output_count > 1u
                         ? TR("Switch its relays, or make one a garage door opener, on its page.")
                     : page.outputs[0].kind == UI_ZW_OUT_DIMMER ? TR("Set its level on its page.")
                                                                : TR("Switch it on its page."),
                     ui_tone_gray());
        const uint32_t packed = (uint32_t)p->node | (uint32_t)p->generation << 16;
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Open it"), ui_tone_blue(), add_open_device,
                      (void *)(intptr_t)(int32_t)packed);
        ui_row_action(body, UI_ICON_CIRCLE_SLASH, TR("Done"), ui_tone_gray(), add_not_now, NULL);
        return;
    }
    ui_section(body, TR("Use it for"));
    const size_t offered = render_use_it_for(body, p->node, p->generation);
    if (offered > 0u) {
        ui_paragraph(body, TR("Tamper and low battery come with the zone: nothing else to set up."),
                    ui_tone_gray());
    }
    ui_row_action(body, UI_ICON_CIRCLE_SLASH, TR("Not now: keep it without a zone"), ui_tone_gray(),
                 add_not_now, NULL);
}

static void render_add_done(const ui_zw_progress_t *p, lv_obj_t *body, char *title, size_t tlen,
                            char *note, size_t nlen)
{
    note[0] = '\0';
    if (s_add_accepted &&
        (p->result == UI_ZW_RES_ADDED_INSECURE || p->result == UI_ZW_RES_DOWNGRADED)) {
        render_added(p, body, title, tlen, note, nlen);
        return;
    }
    switch (p->result) {
    case UI_ZW_RES_ADDED:
        render_added(p, body, title, tlen, note, nlen);
        break;
    case UI_ZW_RES_ADDED_INSECURE: {
        i18n_utf8_copy(title, tlen, TR("Added without encryption")); /* TR: The page title. At most 47 bytes. */
        const char *why;
        switch (p->s2_failure) {
        case UI_ZW_S2_WRONG_PIN: why = TR("The PIN didn't match the device."); break;
        case UI_ZW_S2_TIMEOUT:
            why = TR("The secure connection took too long and didn't finish.");
            break;
        case UI_ZW_S2_CANCELLED: why = TR("You chose to add it without encryption."); break;
        case UI_ZW_S2_DEVICE_STOPPED: why = TR("The device stopped the secure connection."); break;
        case UI_ZW_S2_NO_INFO:
            why = TR("It didn't say what it supports and didn't answer the secure connection: it "
                     "may not have finished joining.");
            break;
        default: why = TR("The secure connection failed."); break;
        }
        char text[256];
        /* TR: joins two sentences on the Added without encryption page. Languages that write no
         * space between sentences (Chinese, Japanese) drop it */
        I18N_FORMAT(text, sizeof(text), TR_C("two sentences", "{0} {1}"), why,
                    TR("Unencrypted, its signal can be copied and it can't guard fire or CO."));
        ui_paragraph(body, text, ui_tone_amber());
        ui_row_link(body, UI_ICON_ROTATE_CCW, ui_tone_blue(), TR("Remove it and try again"),
                   TR("Pair it once to remove it, then add it"), NULL, UI_ICON_CHEVRON_RIGHT,
                   add_remove_and_retry, NULL);
        ui_row_link(body, UI_ICON_CIRCLE_CHECK, ui_tone_gray(), TR("Keep it this way"),
                   TR("Its zones will say it isn't encrypted"), NULL, UI_ICON_CHEVRON_RIGHT,
                   add_keep_it, NULL);
        break;
    }
    case UI_ZW_RES_DOWNGRADED:
        i18n_utf8_copy(title, tlen, TR("Added with weaker security")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                    TR("It supports better security than it got. Someone nearby may have "
                       "interfered. Remove it and add it again."),
                    ui_tone_amber());
        ui_row_action(body, UI_ICON_ROTATE_CCW, TR("Remove it and try again"), ui_tone_blue(),
                     add_remove_and_retry, NULL);
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Keep it"), ui_tone_gray(), add_keep_it, NULL);
        break;
    case UI_ZW_RES_STOPPED: /* stopped before any device joined: nothing is in the network */
        i18n_utf8_copy(title, tlen, TR("Stopped")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                     p->replacing ? TR("Stopped before the new device joined: nothing was replaced.")
                                  : TR("Stopped before a device joined: nothing was added."),
                     ui_tone_gray());
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    case UI_ZW_RES_STILL_ANSWERS: /* §10: replacing a device that still answers */
        i18n_utf8_copy(title, tlen, TR("Not replaced")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                     TR("The old device answered, so it wasn't replaced. If it's still in use, "
                        "keep it; to swap it, remove it the normal way first."),
                     ui_tone_amber());
        ui_row_action(body, UI_ICON_CIRCLE_SLASH, TR("Done"), ui_tone_gray(), add_not_now, NULL);
        break;
    case UI_ZW_RES_REFUSED:
        i18n_utf8_copy(title, tlen, TR("Not replaced")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                     TR("The Z-Wave radio doesn't count the old device as failed yet. Wait a "
                        "minute and try again."),
                     ui_tone_amber());
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    case UI_ZW_RES_SETUP_PAUSED: { /* in the network, its setup not finished */
        i18n_utf8_copy(title, tlen, TR("Setup not finished")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                     TR("It joined, but the panel hasn't finished reading what it can do, so it "
                        "can't be used yet. Its page shows how far it got."),
                     ui_tone_amber());
        ui_row_value(body, TR("Security"), security_full(p->security));
        const uint32_t packed = (uint32_t)p->node | (uint32_t)p->generation << 16;
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Open it"), ui_tone_blue(), add_open_device,
                     (void *)(intptr_t)(int32_t)packed);
        ui_row_action(body, UI_ICON_CIRCLE_SLASH, TR("Done"), ui_tone_gray(), add_not_now, NULL);
        break;
    }
    case UI_ZW_RES_JOIN_FAILED:
        i18n_utf8_copy(title, tlen, TR("Didn't join")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                    TR("The device did not finish joining. Remove it (it may still belong to "
                       "another system) and add it again."),
                    ui_tone_gray());
        ui_row_action(body, UI_ICON_TRASH, TR("Remove a device"), ui_tone_blue(), add_open_remove,
                     NULL);
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    case UI_ZW_RES_ALREADY_ADDED: {
        i18n_utf8_copy(title, tlen, TR("Already added")); /* TR: The page title. At most 47 bytes. */
        char name[64];
        ui_zw_device_page_t page;
        if (S.be.zw_device != NULL && S.be.zw_device(S.be.ctx, p->node, p->generation, &page)) {
            device_name(&page.dev, name, sizeof(name));
            char text[128];
            I18N_FORMAT(text, sizeof(text), TR("This device is already paired: it's {0}."), name);
            ui_paragraph(body, text, ui_tone_gray());
        } else {
            /* TR: a paragraph on the Already added page when the device's name isn't known */
            ui_paragraph(body, TR("This device is already paired."), ui_tone_gray());
        }
        const uint32_t packed = (uint32_t)p->node | (uint32_t)p->generation << 16;
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Open it"), ui_tone_blue(), add_open_device,
                     (void *)(intptr_t)(int32_t)packed);
        break;
    }
    case UI_ZW_RES_CONTROLLER:
        i18n_utf8_copy(title, tlen, TR("Not a sensor")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                    TR("That is another controller, not a sensor: the panel doesn't add "
                       "controllers."),
                    ui_tone_gray());
        ui_row_action(body, UI_ICON_TRASH, TR("Remove it"), ui_tone_blue(), add_remove_direct, NULL);
        break;
    case UI_ZW_RES_NO_REPORTS:
        i18n_utf8_copy(title, tlen, TR("Added, but it doesn't report")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body,
                    TR("It has no lifeline, so it can't tell the panel anything. It can't be "
                       "used for a zone."),
                    ui_tone_amber());
        ui_row_action(body, UI_ICON_TRASH, TR("Remove it"), ui_tone_blue(), add_remove_direct, NULL);
        ui_row_action(body, UI_ICON_CIRCLE_CHECK, TR("Keep it"), ui_tone_gray(), add_not_now, NULL);
        break;
    case UI_ZW_RES_RADIO_RESTARTED: /* ZW-005: the radio restarted, its window closed with it */
        i18n_utf8_copy(title, tlen, TR("Couldn't add it")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body, TR("The Z-Wave radio restarted, so it stopped listening. Try again."),
                     ui_tone_amber());
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    default: /* UI_ZW_RES_FAILED or an unexpected ending: never leave a dead page */
        i18n_utf8_copy(title, tlen, TR("Couldn't add it")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body, TR("Something went wrong. Try again."), ui_tone_gray());
        ui_row_action(body, UI_ICON_REFRESH, TR("Try again"), ui_tone_blue(), add_again, NULL);
        break;
    }
}

static void build_add(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                      size_t nlen)
{
    s_add.open = false;
    ui_zw_status_t st;
    if (!read_status(&st)) {
        i18n_utf8_copy(title, tlen, TR("Add a Z-Wave device")); /* TR: The page title. At most 47 bytes. */
        unavailable_paragraph(body);
        return;
    }
    if (!installer()) {
        i18n_utf8_copy(title, tlen, TR("Add a Z-Wave device")); /* TR: The page title. At most 47 bytes. */
        locked_paragraph(body, LOCKED_ADD);
        return;
    }
    ui_zw_progress_t p;
    read_progress(&p);
    const bool running = p.op == UI_ZW_OP_ADD && p.step != UI_ZW_STEP_DONE &&
                         p.step != UI_ZW_STEP_IDLE;
    const bool this_visit = p.op == UI_ZW_OP_ADD && s_owner == PAGE_ZWAVE_ADD;
    if (!running && !this_visit && !S.page_fresh) {
        /* returned to (back from a page it opened), or its addition dismissed elsewhere: the
         * radio never starts listening by itself (B015 UX-004, UX-005) */
        i18n_utf8_copy(title, tlen, TR("Add a Z-Wave device")); /* TR: The page title. At most 47 bytes. */
        s_owner = 0;
        ui_paragraph(body, TR("Nothing is being added now."), ui_tone_gray());
        ui_row_action(body, UI_ICON_PLUS, TR("Add a device"), ui_tone_blue(), add_again, NULL);
        return;
    }
    if (!running && !this_visit) {
        s_add.arg = arg;
        s_add_accepted = false;
        ui_zw_request_t req;
        add_request(&req);
        const ui_zw_req_result_t r =
            S.be.zw_request != NULL ? S.be.zw_request(S.be.ctx, &req) : UI_ZW_REQ_NO_RADIO;
        if (r != UI_ZW_REQ_OK) {
            i18n_utf8_copy(title, tlen, TR("Add a Z-Wave device")); /* TR: The page title. At most 47 bytes. */
            s_owner = 0;
            refused_paragraph(body, r, true);
            ui_row_action(body, UI_ICON_PLUS, TR("Try again"), ui_tone_blue(), add_again, NULL);
            return;
        }
        read_progress(&p);
    }
    s_owner = PAGE_ZWAVE_ADD;
    /* ZWF_ALLOW/PIN/PROGRESS/DONE set their own */
    /* TR: The page title. At most 47 bytes. */
    i18n_utf8_copy(title, tlen,
                   p.replacing ? TR("Replace a Z-Wave device") : TR("Add a Z-Wave device"));
    switch (add_family(&p)) {
    case ZWF_LISTEN: render_add_listen(&p, body, note, nlen); break;
    case ZWF_ALLOW: render_add_allow(&p, body, title, tlen, note, nlen); break;
    case ZWF_PIN: render_add_pin(&p, body, title, tlen, note, nlen); break;
    case ZWF_PROGRESS: render_add_progress(&p, body, title, tlen, note, nlen); break;
    case ZWF_DONE: render_add_done(&p, body, title, tlen, note, nlen); break;
    }
    s_add.last_step = p.step;
    s_add.open = true;
    s_add.polled = lv_tick_get();
}

static void add_tick(void)
{
    if (!s_add.open || lv_tick_elaps(s_add.polled) < POLL_MS) {
        return;
    }
    s_add.polled = lv_tick_get();
    ui_zw_progress_t p;
    read_progress(&p);
    if (p.step != s_add.last_step) {
        ui_page_rebuild();
        return;
    }
    switch (add_family(&p)) {
    case ZWF_LISTEN:
        if (p.step == UI_ZW_STEP_LISTENING) {
            char note[64];
            /* TR: The page's subtitle while listening. At most 60 bytes. */
            I18N_FORMAT(note, sizeof(note),
                        TRN("Listening · {0} s left", "Listening · {0} s left",
                            (unsigned)p.seconds_left),
                        (unsigned)p.seconds_left);
            ui_page_note(note);
        }
        break;
    case ZWF_ALLOW:
        if (s_allow_countdown != NULL) {
            char left[32];
            /* TR: The time left, minutes then seconds. At most 27 bytes. */
            I18N_FORMAT(left, sizeof(left), TR("{0} min {1} s"), (unsigned)(p.seconds_left / 60u),
                        (unsigned)(p.seconds_left % 60u));
            ui_set_label(s_allow_countdown, "value_label", left);
        }
        break;
    case ZWF_PIN:
        if (s_pin_countdown != NULL) {
            char left[32];
            /* TR: The time left, minutes then seconds. At most 27 bytes. */
            I18N_FORMAT(left, sizeof(left), TR("{0} min {1} s"), (unsigned)(p.seconds_left / 60u),
                        (unsigned)(p.seconds_left % 60u));
            ui_set_label(s_pin_countdown, "value_label", left);
        }
        break;
    case ZWF_PROGRESS: update_add_progress(&p); break;
    default: break;
    }
}

/* ---- §9: SmartStart - sensors registered by their key (network.md §4.2) ----------------------- */

static bool on_top(page_id_t id);

static struct {
    ui_zw_prov_t *list; /* UI_ZW_PROV_MAX, from ui_slow_alloc */
    size_t n;
    uint32_t sig;       /* what the open page shows: rebuilt only when it changes (UI-020) */
    bool open;          /* the list or an entry page is built */
    uint8_t dsk[16];    /* the entry page's sensor */
    uint32_t polled;
} s_smart;

static size_t smart_read(void)
{
    if (s_smart.list == NULL) {
        s_smart.list = ui_slow_alloc(UI_ZW_PROV_MAX * sizeof(*s_smart.list));
    }
    s_smart.n = s_smart.list != NULL && S.be.zw_prov != NULL
                    ? S.be.zw_prov(S.be.ctx, s_smart.list, UI_ZW_PROV_MAX)
                    : 0u;
    return s_smart.n;
}

static bool smart_last_dsk(uint8_t out[16])
{
    if (smart_read() == 0u) {
        return false;
    }
    memcpy(out, s_smart.list[s_smart.n - 1u].dsk, 16u);
    return true;
}

static uint32_t smart_sig(void)
{
    uint32_t h = 2166136261u ^ (uint32_t)s_smart.n;
    for (size_t i = 0; i < s_smart.n; i++) {
        const ui_zw_prov_t *e = &s_smart.list[i];
        const uint8_t b[7] = {e->state, e->failures, e->grant, (uint8_t)e->node, e->dsk[2],
                              e->dsk[15], (uint8_t)e->name[0]};
        for (size_t k = 0; k < sizeof(b); k++) {
            h = (h ^ b[k]) * 16777619u;
        }
    }
    return h;
}

static unsigned smart_group(const uint8_t dsk[16], unsigned g)
{
    return (unsigned)dsk[g * 2u] << 8 | (unsigned)dsk[g * 2u + 1u];
}

/* Its name, its model, or what it is and its key's second group (the first is its PIN: never shown). */
static void smart_name(const ui_zw_prov_t *e, char *out, size_t len)
{
    if (e->name[0] != '\0') {
        i18n_utf8_copy(out, len, e->name);
        return;
    }
    if (e->model[0] != '\0') { /* ZW-024: the model its code names, when the panel knows it */
        i18n_utf8_copy(out, len, e->model);
        return;
    }
    const char *kind = e->generic == 0x10u   ? TR_C("device without a name", "Switch")
                       : e->generic == 0x11u ? TR_C("device without a name", "Dimmer")
                       : e->generic == 0x40u ? TR_C("device without a name", "Keypad")
                                             : TR_C("device without a name", "Sensor");
    char num[8];
    snprintf(num, sizeof(num), "%05u", smart_group(e->dsk, 1u));
    /* TR: the name of a registered sensor that has none: {0} what it is, {1} five digits of its key.
     * At most 30 bytes */
    I18N_FORMAT(out, len, TR_C("device without a name", "{0} {1}"), kind, num);
}

/* Its key as the label prints it, the PIN left out: "xxxxx-35455-...". */
static void smart_key(const uint8_t dsk[16], char *out, size_t len)
{
    size_t at = (size_t)snprintf(out, len, "%s", NOTR("xxxxx") /* the PIN, masked */);
    for (unsigned g = 1u; g < 8u && at < len; g++) {
        at += (size_t)snprintf(&out[at], len - at, "-%05u", smart_group(dsk, g));
    }
}

static const char *smart_state_words(uint8_t state)
{
    switch (state) {
    case UI_ZW_PROV_JOINED: return TR_C("SmartStart sensor state", "Joined");
    case UI_ZW_PROV_STOPPED: return TR_C("SmartStart sensor state", "Stopped");
    case UI_ZW_PROV_OFF: return TR_C("SmartStart sensor state", "Off");
    default: return TR_C("SmartStart sensor state", "Waiting");
    }
}

static void smart_detail(const ui_zw_prov_t *e, char *out, size_t len, lv_color_t *color)
{
    *color = COLOR_TEXT_2;
    switch (e->state) {
    case UI_ZW_PROV_JOINED:
        /* TR: A sensor's detail line. At most 60 bytes. */
        I18N_FORMAT(out, len, TR("In the network: device {0}"), (unsigned)e->node);
        break;
    case UI_ZW_PROV_STOPPED:
        /* TR: A sensor's detail line. At most 63 bytes. */
        i18n_utf8_copy(out, len, TR("Could not secure it 5 times: check its code"));
        *color = ui_tone_amber();
        break;
    case UI_ZW_PROV_OFF:
        /* TR: A sensor's detail line. At most 63 bytes. */
        i18n_utf8_copy(out, len, TR("Turned off: it won't join"));
        break;
    default:
        if (e->long_range) {
            /* TR: A sensor's detail line, " · Long Range" follows. At most 49 bytes. */
            snprintf(out, len, "%s · %s", TR("Power it up: it joins by itself"),
                     NOTR("Long Range") /* Z-Wave term */);
        } else {
            /* TR: A sensor's detail line. At most 63 bytes. */
            i18n_utf8_copy(out, len, TR("Power it up: it joins by itself"));
        }
        break;
    }
    if (e->name[0] != '\0' && e->model[0] != '\0' && e->state != UI_ZW_PROV_STOPPED) {
        /* the name is its own: the model leads the detail line (screens.md §9) */
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s · %s", e->model, out);
        i18n_utf8_copy(out, len, tmp);
    }
}

/* The Z-Wave page's "Waiting to join" row: how many, amber when one stopped. */
static void smart_row_words(void)
{
    if (s_net.smart == NULL) {
        return;
    }
    smart_read();
    size_t waiting = 0u, stopped = 0u;
    for (size_t i = 0; i < s_smart.n; i++) {
        waiting += s_smart.list[i].state == UI_ZW_PROV_WAITING ? 1u : 0u;
        stopped += s_smart.list[i].state == UI_ZW_PROV_STOPPED ? 1u : 0u;
    }
    char value[24];
    if (waiting == 0u) {
        i18n_utf8_copy(value, sizeof(value), TR_C("no sensors waiting", "None"));
    } else {
        count_words((unsigned)waiting, value, sizeof(value));
    }
    set_words(s_net.smart, "value_label", value, COLOR_TEXT_2);
    set_words(s_net.smart, "detail_label",
              stopped > 0u ? TR("One could not be secured: check its code")
                           : TR("Sensors registered by their QR code"),
              stopped > 0u ? ui_tone_amber() : COLOR_TEXT_2);
}

static const char *smart_refused(ui_zw_prov_result_t r)
{
    switch (r) {
    case UI_ZW_PROV_BAD_CODE: return TR("That isn't a 40-digit key: check it");
    case UI_ZW_PROV_CHECKSUM: return TR("That code is damaged: scan or type it again");
    case UI_ZW_PROV_FULL: return TR("32 sensors are registered: forget one first");
    case UI_ZW_PROV_S2_ONLY: return TR("That code can't join by itself: add it with its PIN");
    case UI_ZW_PROV_NOT_ALLOWED: return TR("Only in installer mode");
    case UI_ZW_PROV_JOINED_ALREADY: return TR("That device is in the network already");
    default: return TR("It's no longer registered");
    }
}

static void smart_typed(const char *text)
{
    if (S.be.zw_prov_register == NULL) {
        return;
    }
    const ui_zw_prov_result_t r = S.be.zw_prov_register(S.be.ctx, text, "");
    if (r != UI_ZW_PROV_ADDED && r != UI_ZW_PROV_UPDATED) {
        if (r != UI_ZW_PROV_JOINED_ALREADY) {
            ui_beep(UI_BEEP_ERROR);
        }
        ui_toast(smart_refused(r));
    }
    ui_page_rebuild(); /* the new row says the rest */
}

static void smart_register(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Its 40-digit key"), TR("On its label or box, next to the QR code"), "",
                       UI_TEXT_NUMBER, 40, smart_typed);
}

static void smart_open_entry(void *ctx)
{
    const size_t i = (size_t)(intptr_t)ctx;
    if (i < s_smart.n) {
        const ui_zw_prov_t *j = &s_smart.list[i];
        if (j->state == UI_ZW_PROV_JOINED && j->node != 0u &&
            page_builder(PAGE_ZWAVE_DEVICE) != NULL) { /* ZW-024: a joined sensor's line is its device */
            ui_page_open(PAGE_ZWAVE_DEVICE, (int32_t)((uint32_t)j->node | (uint32_t)j->generation << 16));
            return;
        }
        memcpy(s_smart.dsk, s_smart.list[i].dsk, sizeof(s_smart.dsk));
        ui_page_open(PAGE_ZWAVE_SMART_ENTRY, 0);
    }
}

static void build_smart(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                        size_t nlen)
{
    (void)arg;
    i18n_utf8_copy(title, tlen, TR("Waiting to join")); /* TR: The page title. At most 47 bytes. */
    /* TR: The page's subtitle. At most 159 bytes. */
    i18n_utf8_copy(note, nlen, TR("Sensors registered by their QR code"));
    smart_read();
    s_smart.sig = smart_sig();
    s_smart.open = true;
    s_smart.polled = lv_tick_get();
    for (size_t i = 0; i < s_smart.n; i++) {
        const ui_zw_prov_t *e = &s_smart.list[i];
        char name[40], detail[64];
        lv_color_t color;
        smart_name(e, name, sizeof(name));
        smart_detail(e, detail, sizeof(detail), &color);
        const lv_color_t tone = e->state == UI_ZW_PROV_JOINED    ? ui_tone_green()
                                : e->state == UI_ZW_PROV_STOPPED ? ui_tone_amber()
                                : e->state == UI_ZW_PROV_OFF     ? ui_tone_gray()
                                                                 : ui_tone_blue();
        lv_obj_t *row = ui_row_link(body, UI_ICON_PLUG, tone, name, detail,
                                    smart_state_words(e->state), UI_ICON_CHEVRON_RIGHT,
                                    smart_open_entry, (void *)(intptr_t)i);
        set_words(row, "detail_label", detail, color);
    }
    if (s_smart.n == 0u) {
        ui_paragraph(body, TR("No sensor is registered."), ui_tone_gray());
    }
    if (installer()) {
        ui_row_link(body, UI_ICON_KEY_ROUND, ui_tone_blue(), TR("Register a sensor"),
                    TR("Type its 40-digit key, or scan it with a phone"), NULL, UI_ICON_CHEVRON_RIGHT,
                    smart_register, NULL);
    } else {
        ui_paragraph(body, TR("Registering a sensor is done in installer mode."), ui_tone_gray());
    }
    ui_paragraph(body,
                 TR("A registered sensor joins by itself when it is powered, secured with S2: no "
                    "buttons to press. Scan QR codes with a phone in the Panel Manager (Z-Wave "
                    "page)."),
                 ui_tone_gray());
}

static const ui_zw_prov_t *smart_find(void)
{
    for (size_t i = 0; i < s_smart.n; i++) {
        if (memcmp(s_smart.list[i].dsk, s_smart.dsk, sizeof(s_smart.dsk)) == 0) {
            return &s_smart.list[i];
        }
    }
    return NULL;
}

static void smart_change(uint8_t op, uint8_t arg)
{
    if (S.be.zw_prov_change == NULL) {
        return;
    }
    const ui_zw_prov_result_t r = S.be.zw_prov_change(S.be.ctx, s_smart.dsk, op, arg);
    if (r != UI_ZW_PROV_UPDATED) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(smart_refused(r));
    }
}

static void smart_on(void *ctx)
{
    (void)ctx;
    smart_change(UI_ZW_PROV_ON, 0u);
    ui_page_rebuild();
}

static void smart_off(void *ctx)
{
    (void)ctx;
    smart_change(UI_ZW_PROV_OFF_OP, 0u);
    ui_page_rebuild();
}

static void smart_forget_go(void)
{
    smart_change(UI_ZW_PROV_FORGET, 0u);
    ui_page_back();
}

static void smart_forget(void *ctx)
{
    (void)ctx;
    const ui_zw_prov_t *e = smart_find();
    ui_confirm(TR("Forget this sensor?"),
               e != NULL && e->state == UI_ZW_PROV_JOINED
                   ? TR("It stays in the network. If it is reset later, it won't join by itself.")
                   : TR("It won't join by itself any more. Register it again to change that."),
               TR("Forget it"), smart_forget_go);
}

/* Its keys: a subset of what its code asked for (all three for a typed key), never none. */
static void smart_key_toggled(bool on, void *ctx)
{
    const uint8_t bit = (uint8_t)(uintptr_t)ctx;
    const ui_zw_prov_t *e = smart_find();
    if (e == NULL) {
        return;
    }
    const uint8_t grant = on ? (uint8_t)(e->grant | bit) : (uint8_t)(e->grant & (uint8_t)~bit);
    if (grant == 0u) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("It needs at least one"));
    } else {
        smart_change(UI_ZW_PROV_GRANT, grant);
    }
    ui_page_rebuild();
}

static void build_smart_entry(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                              size_t nlen)
{
    (void)arg;
    smart_read();
    s_smart.sig = smart_sig();
    s_smart.open = true;
    s_smart.polled = lv_tick_get();
    const ui_zw_prov_t *e = smart_find();
    if (e == NULL) {
        i18n_utf8_copy(title, tlen, TR("Registered sensor")); /* TR: The page title. At most 47 bytes. */
        ui_paragraph(body, TR("It is no longer registered."), ui_tone_gray());
        return;
    }
    char name[40], detail[64], key[48];
    lv_color_t color;
    smart_name(e, name, sizeof(name));
    i18n_utf8_copy(title, tlen, name);
    i18n_utf8_copy(note, nlen, smart_state_words(e->state));
    smart_detail(e, detail, sizeof(detail), &color);
    ui_paragraph(body, detail, e->state == UI_ZW_PROV_STOPPED ? ui_tone_amber() : ui_tone_gray());
    smart_key(e->dsk, key, sizeof(key));
    char key_line[64];
    /* TR: A line of its own. At most 16 bytes (the key follows). */
    I18N_FORMAT(key_line, sizeof(key_line), TR("Key {0}"), key);
    ui_paragraph(body, key_line, COLOR_TEXT_2);
    const bool can = installer();
    const bool joined = e->state == UI_ZW_PROV_JOINED;
    /* ZW-024: once joined, the class it got instead of what it may have */
    ui_section(body, joined ? TR("Security it got") : TR("Security it may have"));
    static const struct {
        uint8_t bit;
        const char *title, *detail;
    } KEYS[] = {
        {UI_ZW_KEY_S2_ACCESS, TR_NOOP("Access control"), TR_NOOP("For locks, keypads and garage doors")},
        {UI_ZW_KEY_S2_AUTH, TR_NOOP("With its PIN"), TR_NOOP("Its key checked")},
        {UI_ZW_KEY_S2_UNAUTH, TR_NOOP("Without a PIN"),
         TR_NOOP("Encrypted; its key not checked")},
    };
    const uint8_t may = e->requested != 0u ? e->requested : (uint8_t)0x07u;
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if ((may & KEYS[i].bit) == 0u) {
            continue;
        }
        const bool on = (e->grant & KEYS[i].bit) != 0u;
        if (joined && !on) {
            continue;
        }
        if (can && !joined) {
            ui_row_toggle(body, TR(KEYS[i].title), TR(KEYS[i].detail), on, smart_key_toggled,
                          (void *)(uintptr_t)KEYS[i].bit);
        } else {
            ui_row_value(body, TR(KEYS[i].title),
                         on ? TR_C("yes or no", "Yes") : TR_C("yes or no", "No"));
        }
    }
    if (joined && (e->grant & 0x07u) == 0u) {
        ui_paragraph(body, TR("It joined without S2 security."), ui_tone_gray());
    }
    if (!can) {
        ui_paragraph(body, TR("Changing it is done in installer mode."), ui_tone_gray());
        return;
    }
    if (e->state == UI_ZW_PROV_OFF || e->state == UI_ZW_PROV_STOPPED) {
        ui_row_action(body, UI_ICON_PLUG, TR("Turn it on"), ui_tone_blue(), smart_on, NULL);
    } else if (e->state == UI_ZW_PROV_WAITING) {
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Turn it off"), ui_tone_gray(), smart_off, NULL);
    }
    ui_row_action(body, UI_ICON_TRASH, TR("Forget it"), ui_tone_red(), smart_forget, NULL);
}

/* Once a second while a SmartStart page is open: rebuilt only when the list changed. */
static void smart_tick(void)
{
    if (!s_smart.open || lv_tick_elaps(s_smart.polled) < POLL_MS) {
        return;
    }
    s_smart.polled = lv_tick_get();
    if (!on_top(PAGE_ZWAVE_SMART) && !on_top(PAGE_ZWAVE_SMART_ENTRY)) {
        s_smart.open = false;
        return;
    }
    smart_read();
    if (smart_sig() != s_smart.sig) {
        ui_page_rebuild();
    }
}

/* ---- the page table, the refresh ------------------------------------------------------------- */

void ui_zwave_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                    size_t nlen)
{
    const page_fn build = page_builder(id);
    if (build != NULL) {
        build(arg, body, title, tlen, note, nlen);
    }
}

/* ---- the Troubles list (screens.md section 10, ZW-019) --------------------------------------- */

/* The Z-Wave radio has the problem the sensor radio trouble is about. */
static bool radio_in_trouble(const ui_zw_status_t *st)
{
    return st->jammed || st->radio == UI_ZW_RADIO_NOT_ANSWERING ||
           st->radio == UI_ZW_RADIO_UNUSABLE || st->radio == UI_ZW_RADIO_FOREIGN;
}

bool ui_zwave_trouble_line(alarm_sys_trouble_t t, ui_line_t *line)
{
    ui_zw_status_t st;
    if (t != ALARM_TRBL_SENSOR_RADIO || !read_status(&st) || !radio_in_trouble(&st)) {
        return false;
    }
    /* TR: the title of the sensor radio trouble's row when the Z-Wave radio has the problem. At
     * most 63 bytes */
    i18n_utf8_copy(line->title, sizeof(line->title), TR("Z-Wave radio problem"));
    /* TR: the second line of that row: it opens the Z-Wave page. At most 95 bytes */
    i18n_utf8_copy(line->detail, sizeof(line->detail),
                   TR("Wireless sensors may not be heard · Tap to see the radio"));
    return true;
}

/* As the Zigbee row's (ui_zigbee.c trouble_open): the Z-Wave page on top of the installer menu;
 * outside installer mode the locked Installer page takes the code first, and the Z-Wave page
 * follows (trouble_follow). */
static bool s_after_code;

static void trouble_open(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
    ui_settings_section(NOTR("installer")); /* an id, not words on the screen */
    if (S.program) {
        s_after_code = false;
        ui_page_open(PAGE_ZWAVE, 0);
        return;
    }
    s_after_code = true;
    /* TR: a toast when the sensor radio trouble opens the locked Installer page. At most 63 bytes */
    ui_toast(TR("Enter the installer code to see the radio"));
}

static void trouble_follow(void)
{
    if (!s_after_code) {
        return;
    }
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS || S.depth != 2u ||
        S.pages[1].id != PAGE_INSTALLER) { /* About, then Installer (BL-189) */
        s_after_code = false;
        return;
    }
    if (S.program) {
        s_after_code = false;
        ui_page_open(PAGE_ZWAVE, 0);
    }
}

void ui_zwave_trouble_link(lv_obj_t *row, bool link)
{
    if (row == NULL) {
        return;
    }
    if (lv_obj_remove_event_cb(row, trouble_open)) {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE); /* the row is reused for another line */
    }
    if (link) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, trouble_open, LV_EVENT_CLICKED, NULL);
    }
}

void ui_zwave_tick(void)
{
    trouble_follow();
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return;
    }
    ui_zw_status_t st;
    if (s_menu_row != NULL && lv_tick_elaps(s_menu_polled) >= POLL_MS) {
        s_menu_polled = lv_tick_get();
        if (read_status(&st)) {
            menu_words(s_menu_row, &st);
        }
    }
    if (s_net.radio != NULL && lv_tick_elaps(s_net.polled) >= POLL_MS) {
        s_net.polled = lv_tick_get();
        show_status();
        show_devices();
    }
    add_tick();
    remove_tick();
    device_tick();
    radio_tick();
    smart_tick();
}

/* Leaving the add or remove page stops it; leaving one that already shows a result clears it
 * (screens.md §4 "Leaving the page stops listening", §7). A same-page refresh (the 1 s tick,
 * ui_page_rebuild) must not: S.pages[] already names the page about to be built, and the view
 * subject already carries a change away from Settings entirely (ui_app.c's view_changed runs
 * this from ui_settings_leave() only after subject_view no longer reads VIEW_SETTINGS). */
static bool on_top(page_id_t id)
{
    return lv_subject_get_int(&subject_view) == VIEW_SETTINGS && S.depth > 0u &&
           S.pages[S.depth - 1u].id == id;
}

static void ask_quietly(uint8_t kind)
{
    if (S.be.zw_request != NULL) {
        ui_zw_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = kind;
        req.user = S.auth_user;
        (void)S.be.zw_request(S.be.ctx, &req); /* leaving: nothing to say if it was over */
    }
}

static void stop_if_leaving(void)
{
    /* the device page's "reset, broken or gone" outcome belongs to that visit */
    if (!on_top(PAGE_ZWAVE_DEVICE) && !on_top(PAGE_ZWAVE_REMOVE)) {
        s_clean.result = UI_ZW_RES_NONE;
    }
    if (!on_top(PAGE_ZWAVE_DEVICE) && !on_top(PAGE_ZWAVE_ADD)) {
        memset(&s_retry, 0, sizeof(s_retry));
    }
    if (s_owner == 0 || on_top(s_owner) || S.be.zw_progress == NULL) {
        return;
    }
    const uint8_t op = s_owner == PAGE_ZWAVE_ADD ? UI_ZW_OP_ADD : UI_ZW_OP_REMOVE;
    s_owner = 0;
    ui_zw_progress_t p;
    read_progress(&p);
    if (p.op != op || p.step == UI_ZW_STEP_IDLE) {
        return; /* not what that page ran: someone else's, left alone */
    }
    if (p.step == UI_ZW_STEP_STARTING || p.step == UI_ZW_STEP_LISTENING ||
        p.step == UI_ZW_STEP_READING) {
        ask_quietly(UI_ZW_REQ_STOP);
        read_progress(&p); /* a stop ends it at once: its "stopped" is nobody's to read */
    } else if (p.step != UI_ZW_STEP_DONE) {
        return; /* CONNECTING, ASK_ALLOW, ASK_PIN, SECURING: no STOP for these; it goes on and
                 * reopening the page follows it (build_add/build_remove read the running op) */
    }
    if (p.op == op && p.step == UI_ZW_STEP_DONE) {
        ask_quietly(UI_ZW_REQ_DISMISS);
    }
}

void ui_zwave_stop(void)
{
    stop_if_leaving();
    s_menu_row = NULL;
    s_net.radio = NULL;
    s_net.list = NULL;
    s_net.empty = NULL;
    s_net.region = NULL;
    s_net.check_in = NULL;
    s_net.nshown = 0;
    s_add.open = false;
    memset(&s_prog, 0, sizeof(s_prog));
    s_pin_countdown = NULL;
    s_rm.open = false;
    s_dev.open = false;
    s_dev.last_heard = NULL;
    s_dev.check_in = NULL;
    s_radio.open = false;
    s_radio.state = NULL;
    s_radio.noise = NULL;
    s_net.smart = NULL;
    s_net.routes_note = NULL;
    memset(s_net.routes_item, 0, sizeof(s_net.routes_item));
    s_net.routes = NULL;
    s_smart.open = false;
}
