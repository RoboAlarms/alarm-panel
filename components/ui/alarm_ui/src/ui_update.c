/*
 * ui_update.c - Settings > About > Software update (docs/features/13; BL-131): what is waiting,
 * one Install button, Check again; and About's row for it.
 *
 * The page opens for the installer and the system master only (ui_settings.c asks for the code;
 * BL-188). About's row says a little to anyone. The panel installs only when everything is
 * disarmed and quiet, so "Install" can be pressed at any time.
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define POLL_MS 1000u

static lv_obj_t *s_row; /* the status row of the open page, NULL when none */
static ui_fw_status_t s_shown;
static uint32_t s_polled;
static void fwsrv_tick(void);
static bool s_asking;   /* the "Stop the update?" question is up */
static uint8_t s_step;  /* the step the run was at (a stop leaves the steps where it found them) */

/* What the page shows (and what the bar's banner and the tag follow). */
typedef enum {
    V_PLAIN = 0, /* nothing runs and nothing to report: up to date, offered, not set up ... */
    V_CHECKING,  /* a plain check: not a run */
    V_DOWNLOAD,
    V_VERIFY,
    V_WAIT,
    V_INSTALL,
    V_STOPPING,
    V_BACK,      /* the self-test failed: restarting with the old version */
    V_WIFI,
    V_FILE,
    V_SELFTEST,
    V_FAILED,
    V_STOPPED,
    V_ROLLED,
    V_UPDATED,
} view_t;

#define UPDATED_SHOWN_S (24u * 3600u) /* "Updated to ..." is told this long */

static void read_status(ui_fw_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->percent = -1;
    if (S.be.fw_status != NULL) {
        S.be.fw_status(S.be.ctx, st);
    }
}

static bool updated_fresh(const ui_fw_status_t *st)
{
    if (st->last.at == 0u || !ui_clock_known() || S.be.epoch_ms == NULL) {
        return true; /* no time to measure by: it stays until a newer result */
    }
    const int64_t now = S.be.epoch_ms(S.be.ctx) / 1000;
    return now < (int64_t)st->last.at || now - (int64_t)st->last.at < (int64_t)UPDATED_SHOWN_S;
}

static view_t view_of(const ui_fw_status_t *st)
{
    switch (st->stage) {
    case UI_FW_STAGE_DOWNLOADING: return V_DOWNLOAD;
    case UI_FW_STAGE_VERIFYING: return V_VERIFY;
    case UI_FW_STAGE_WAITING: return V_WAIT;
    case UI_FW_STAGE_INSTALLING: return V_INSTALL;
    case UI_FW_STAGE_STOPPING: return V_STOPPING;
    case UI_FW_STAGE_ROLLING_BACK: return V_BACK;
    case UI_FW_STAGE_WIFI_MODULE: return V_WIFI;
    case UI_FW_STAGE_FILE: return V_FILE;
    default: break;
    }
    if (st->confirming) {
        return V_SELFTEST;
    }
    if (st->phase == UI_FW_CHECKING) {
        return V_CHECKING;
    }
    if (st->phase == UI_FW_FAILED) {
        return V_FAILED;
    }
    switch (st->last.result) {
    case UI_FW_LAST_STOPPED: return V_STOPPED;
    case UI_FW_LAST_FAILED: return V_FAILED;
    case UI_FW_LAST_ROLLED_BACK: return V_ROLLED;
    case UI_FW_LAST_UPDATED:
        if (!st->available && updated_fresh(st)) {
            return V_UPDATED;
        }
        break;
    default: break;
    }
    return st->rolled_back[0] != '\0' ? V_ROLLED : V_PLAIN;
}

/* A run (Install to the end of the self-test), as the tag and the banner count it. A developer's
 * file is not one of the update's own. */
static bool is_run(view_t v)
{
    return v >= V_DOWNLOAD && v <= V_SELFTEST && v != V_FILE;
}

static bool stoppable(view_t v, const ui_fw_status_t *st)
{
    return st->release && (v == V_DOWNLOAD || v == V_VERIFY || v == V_WAIT);
}

/* About's tag in the Settings list: 1 Update (a version is offered), 2 Updating (a run), 3 Rolled
 * back (BL-131, BL-188). */
void ui_update_tag(void)
{
    ui_fw_status_t st;
    read_status(&st);
    const view_t v = view_of(&st);
    ui_set_int(&subject_update_waiting,
               is_run(v) || v == V_FILE ? 2 : v == V_ROLLED ? 3 : st.available ? 1 : 0);
}

/* ---- the status bar's banner (BL-188) --------------------------------------------------------- */

static uint8_t s_ban;       /* 0 none, 1 blue, 2 amber (going back) */
static char s_ban_text[96];
static uint32_t s_ban_polled;

uint8_t ui_update_banner(void)
{
    return s_ban;
}

/* Reads the run once a second and sets the words only when the step changed: a banner that talks
 * steadily, never a percent (UI-020). */
void ui_update_banner_tick(void)
{
    if (s_ban_polled != 0u && lv_tick_elaps(s_ban_polled) < POLL_MS) {
        return;
    }
    s_ban_polled = lv_tick_get() | 1u;
    ui_fw_status_t st;
    read_status(&st);
    const view_t v = view_of(&st);
    char text[96] = "";
    const char *glyph = UI_ICON_DOWNLOAD;
    uint8_t kind = 1;
    switch (v) {
    case V_DOWNLOAD:
        /* TR: the status bar while an update downloads. At most 63 bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Downloading an update"));
        break;
    case V_VERIFY:
        /* TR: the status bar while the downloaded update is checked. At most 63 bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Checking the download"));
        glyph = UI_ICON_SHIELD_CHECK;
        break;
    case V_WAIT:
        /* TR: the status bar while a downloaded update waits for everything to be disarmed.
         * At most 63 bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Update waiting · installs once disarmed"));
        glyph = UI_ICON_CLOCK;
        break;
    case V_INSTALL:
        /* TR: the status bar in the seconds before the panel restarts to install an update.
         * At most 63 bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Restarting to install the update"));
        glyph = UI_ICON_ROTATE_CCW;
        break;
    case V_STOPPING:
        /* TR: the status bar while an update is being stopped. At most 63 bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Stopping the update"));
        glyph = UI_ICON_CIRCLE_X;
        break;
    case V_SELFTEST:
        /* TR: the status bar while the new version tests itself after an update. At most 63
         * bytes */
        i18n_utf8_copy(text, sizeof(text), TR("Self-test after the update"));
        glyph = UI_ICON_SHIELD_CHECK;
        break;
    case V_BACK:
        /* TR: the status bar when the self-test failed and the panel restarts with the old
         * version; {0} is that version. At most 63 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Going back to version {0} · restarting"), st.previous);
        kind = 2;
        break;
    case V_WIFI:
        if (st.phase == UI_FW_WAITING) {
            /* TR: the status bar when the Wi-Fi module's update is ready and waits for
             * everything to be disarmed. At most 63 bytes */
            i18n_utf8_copy(text, sizeof(text), TR("Wi-Fi module update ready · installs once disarmed"));
            glyph = UI_ICON_CLOCK;
        } else {
            /* TR: the status bar while the Wi-Fi module is updated. At most 63 bytes */
            i18n_utf8_copy(text, sizeof(text), TR("Updating the Wi-Fi module"));
            glyph = UI_ICON_WIFI;
        }
        break;
    default:
        kind = 0;
        break;
    }
    if (kind != s_ban || (kind != 0u && strcmp(text, s_ban_text) != 0)) {
        s_ban = kind;
        snprintf(s_ban_text, sizeof(s_ban_text), "%s", text);
        if (kind != 0u) {
            ui_set_str(&subject_update_banner, text);
            ui_set_str(&subject_update_banner_glyph, glyph);
        }
        ui_bar_banner_update();
    }
}

static bool busy(const ui_fw_status_t *st)
{
    return st->phase == UI_FW_CHECKING || st->stage != UI_FW_STAGE_NONE || st->confirming;
}

/* The percent done, whole: the catalog's own, else worked out from the bytes; -1 = not known. */
static int percent_of(const ui_fw_status_t *st)
{
    if (st->percent >= 0) {
        return st->percent > 100 ? 100 : st->percent;
    }
    if (st->size != 0u) {
        const uint32_t p = (uint32_t)(((uint64_t)st->bytes * 100u) / st->size);
        return p > 100u ? 100 : (int)p;
    }
    return -1;
}

static void pct_text(int pct, char *out, size_t len)
{
    I18N_FORMAT(out, len, TR_C("percent", "{0} %"), pct);
}

/* "1.1 of 2.7 MB · 42 %": in tenths of a megabyte, in the region's numbers. */
static void download_words(const ui_fw_status_t *st, char *out, size_t len)
{
    if (st->size == 0u) {
        /* TR: Software update, the line under the download before the first bytes (the panel asks
         * the server first). At most 63 bytes */
        i18n_utf8_copy(out, len, TR("Starting the download"));
        return;
    }
    char done[16], total[16], pct[16];
    i18n_number(done, sizeof(done), ui_region(), (int64_t)(((uint64_t)st->bytes * 10u + (1u << 19)) >> 20), 1u);
    i18n_number(total, sizeof(total), ui_region(), (int64_t)(((uint64_t)st->size * 10u + (1u << 19)) >> 20), 1u);
    pct_text(percent_of(st), pct, sizeof(pct));
    /* TR: Software update, the download's progress: {0} megabytes done of {1}, then {2}, the
     * percent. At most 63 bytes */
    I18N_FORMAT(out, len, TR("{0} of {1} MB · {2}"), done, total, pct);
}

enum { WHY_OTHER = 0, WHY_INSTALLER, WHY_ARMED, WHY_ALARM, WHY_WALK, WHY_AGAIN };

static int why_kind(const char *reason)
{
    if (strcmp(reason, NOTR("Installer mode is on")) == 0) return WHY_INSTALLER;
    if (strcmp(reason, NOTR("The system is armed")) == 0) return WHY_ARMED;
    if (strcmp(reason, NOTR("An alarm is sounding")) == 0) return WHY_ALARM;
    if (strcmp(reason, NOTR("A walk test is running")) == 0) return WHY_WALK;
    if (strcmp(reason, NOTR("Trying again shortly")) == 0) return WHY_AGAIN;
    return WHY_OTHER;
}

/* What ends the wait, for the line under "Waiting to install" and the paragraph below Stop. */
static const char *wait_ends(int why)
{
    switch (why) {
    /* TR: Software update, waiting for the restart: what ends the wait; each is joined with a
     * middle dot to the reason. At most 47 bytes */
    case WHY_INSTALLER: return TR("Installs once installer mode is off");
    case WHY_ARMED: return TR("Installs once everything is disarmed");
    case WHY_ALARM: return TR("Installs once the alarm is silenced");
    case WHY_WALK: return TR("Installs once the walk test ends");
    case WHY_AGAIN: return TR("Installs within 30 seconds");
    default: return TR("Installs once everything is disarmed");
    }
}

static const char *wait_paragraph(int why)
{
    switch (why) {
    /* TR: Software update, the paragraph under Stop while the update waits to restart. At most
     * 111 bytes */
    case WHY_INSTALLER:
        return TR("Leave installer mode and the panel restarts to install it. It takes about a minute.");
    case WHY_ARMED:
        return TR("Disarm everything and the panel restarts to install it. It takes about a minute.");
    case WHY_ALARM:
        return TR("Silence the alarm and the panel restarts to install it. It takes about a minute.");
    case WHY_WALK:
        return TR("End the walk test and the panel restarts to install it. It takes about a minute.");
    case WHY_AGAIN:
        return TR("The system got busy as it was about to restart. It tries again within 30 seconds.");
    default:
        return TR("The panel restarts to install it once everything is disarmed. It takes about a minute.");
    }
}

/* The state in words, a glyph and a tone; `bar` = the level bar's percent to show, -1 = no bar.
 * Each title lands in a 64-byte buffer and each detail in a 112-byte one. */
static void status_words(const ui_fw_status_t *st, char *title, size_t tlen, char *detail,
                         size_t dlen, const char **glyph, lv_color_t *tone, int *bar)
{
    *glyph = UI_ICON_REFRESH;
    *tone = ui_tone_blue();
    *bar = -1;
    const char *target = st->target[0] != '\0' ? st->target : st->latest;
    switch (view_of(st)) {
    case V_CHECKING:
        /* TR: Settings > About > Software update, the status while it looks for a new version.
         * At most 63 bytes */
        i18n_utf8_copy(title, tlen, TR("Checking for updates"));
        /* TR: the line under "Checking for updates". At most 111 bytes */
        i18n_utf8_copy(detail, dlen, TR("Asking the update server"));
        break;
    case V_DOWNLOAD:
        /* TR: Software update status while the new version downloads; {0} is the version. At most
         * 50 bytes */
        I18N_FORMAT(title, tlen, TR("Downloading version {0}"), target);
        download_words(st, detail, dlen);
        *glyph = UI_ICON_DOWNLOAD;
        *bar = st->size == 0u ? 0 : percent_of(st);
        break;
    case V_VERIFY:
        /* TR: Software update status while the downloaded file is checked. At most 63 bytes */
        i18n_utf8_copy(title, tlen, TR("Checking the download"));
        /* TR: the line under "Checking the download". At most 111 bytes */
        i18n_utf8_copy(detail, dlen, TR("Its signature and checksum · a few seconds"));
        *glyph = UI_ICON_SHIELD_CHECK;
        *bar = 100;
        break;
    case V_WAIT: {
        /* TR: Software update status: the new version is downloaded and waits; {0} is the
         * version. At most 50 bytes */
        I18N_FORMAT(title, tlen, TR("Waiting to install version {0}"), target);
        const int why = why_kind(st->reason);
        if (st->reason[0] != '\0') {
            char reason[64];
            snprintf(reason, sizeof(reason), "%s", ui_answer("update", st->reason)); /* panel_ota.c's and fw.c's reasons */
            snprintf(detail, dlen, "%s · %s", wait_ends(why), reason);
        } else {
            snprintf(detail, dlen, "%s", wait_ends(why));
        }
        *glyph = UI_ICON_CLOCK;
        *tone = ui_tone_amber();
        break;
    }
    case V_INSTALL:
        /* TR: Software update status while the panel restarts to install; {0} is the version. At
         * most 50 bytes */
        I18N_FORMAT(title, tlen, TR("Restarting with version {0}"), target);
        /* TR: the line under "Restarting with version ...". At most 111 bytes */
        i18n_utf8_copy(detail, dlen, TR("The screen is back in about a minute"));
        *glyph = UI_ICON_ROTATE_CCW;
        break;
    case V_STOPPING:
        /* TR: Software update status while a stop lands. At most 63 bytes */
        i18n_utf8_copy(title, tlen, TR("Stopping the update"));
        /* TR: the line under "Stopping the update". At most 111 bytes */
        i18n_utf8_copy(detail, dlen,
                       TR("Nothing is changed · a few seconds, up to a minute on a slow connection"));
        *glyph = UI_ICON_CIRCLE_X;
        *tone = ui_tone_gray();
        break;
    case V_BACK:
        /* TR: Software update status when the self-test failed and the panel restarts with the
         * old version; {0} is that version. At most 50 bytes */
        I18N_FORMAT(title, tlen, TR("Going back to version {0}"), st->previous);
        /* TR: the line under "Going back to version ...". At most 111 bytes */
        i18n_utf8_copy(detail, dlen, TR("The self-test failed · restarting"));
        *glyph = UI_ICON_TRIANGLE_ALERT;
        *tone = ui_tone_amber();
        break;
    case V_WIFI: {
        const int pct = percent_of(st);
        if (st->phase == UI_FW_WAITING) {
            /* TR: Software update status when the Wi-Fi module's update is ready. At most 63
             * bytes */
            i18n_utf8_copy(title, tlen, TR("Wi-Fi module update ready"));
            /* TR: the line under it. At most 111 bytes */
            i18n_utf8_copy(detail, dlen, TR("Installs once everything is disarmed"));
            *glyph = UI_ICON_CLOCK;
            *tone = ui_tone_amber();
        } else {
            /* TR: Software update status while the Wi-Fi module is updated. At most 63 bytes */
            i18n_utf8_copy(title, tlen, TR("Updating the Wi-Fi module"));
            if (pct >= 0) {
                char p[16];
                pct_text(pct, p, sizeof(p));
                /* TR: the line under "Updating the Wi-Fi module"; {0} is the percent done. At
                 * most 111 bytes */
                I18N_FORMAT(detail, dlen,
                            TR("{0} · the panel restarts when it's done, once everything is disarmed"), p);
            } else {
                /* TR: the line under "Updating the Wi-Fi module". At most 111 bytes */
                i18n_utf8_copy(detail, dlen,
                               TR("The panel restarts when it's done, once everything is disarmed"));
            }
            *glyph = UI_ICON_WIFI;
            *bar = pct >= 0 ? pct : 0;
        }
        break;
    }
    case V_FILE: {
        const int pct = percent_of(st);
        /* TR: Software update status while a developer's file is installed. At most 63 bytes */
        i18n_utf8_copy(title, tlen, TR("Installing a developer file"));
        if (pct >= 0) {
            pct_text(pct, detail, dlen);
            *bar = pct;
        } else {
            detail[0] = '\0';
        }
        *glyph = UI_ICON_DOWNLOAD;
        break;
    }
    case V_SELFTEST:
        /* TR: Software update status after a restart, while the new version tests itself; {0} is
         * the version. At most 50 bytes */
        I18N_FORMAT(title, tlen, TR("Self-test of version {0}"), st->running);
        /* TR: the line under "Self-test of version ...". At most 111 bytes */
        i18n_utf8_copy(detail, dlen, TR("Usually a minute, at most three"));
        *glyph = UI_ICON_SHIELD_CHECK;
        break;
    case V_FAILED:
        if (st->phase == UI_FW_FAILED ? st->failed_install : true) {
            /* TR: Software update status after an update failed. At most 63 bytes */
            i18n_utf8_copy(title, tlen, TR("Update failed"));
            const char *why = st->phase == UI_FW_FAILED ? st->reason : st->last.reason;
            char reason[64];
            snprintf(reason, sizeof(reason), "%s",
                     why[0] != '\0' ? ui_answer("update", why) : TR("Try again later"));
            /* TR: the line under "Update failed": the reason, then this. At most 111 bytes */
            I18N_FORMAT(detail, dlen, TR("{0} · nothing was changed"), reason);
        } else {
            /* TR: Software update status after a check for a new version failed. At most 63
             * bytes */
            i18n_utf8_copy(title, tlen, TR("Couldn't check for updates"));
            /* TR: the line under it: the panel's reason, or this. At most 111 bytes */
            i18n_utf8_copy(detail, dlen,
                           st->reason[0] ? ui_answer("update", st->reason) : TR("Try again later"));
        }
        *glyph = UI_ICON_CIRCLE_X;
        *tone = ui_tone_red();
        break;
    case V_STOPPED: {
        /* TR: Software update status after an update was stopped. At most 63 bytes */
        i18n_utf8_copy(title, tlen, TR("Update stopped"));
        /* TR: Software update, who stopped it when no one was signed in. At most 23 bytes */
        const char *by = st->last.by[0] != '\0' ? st->last.by : TR("the panel");
        /* TR: the line under "Update stopped"; {0} is who stopped it (a name, "the installer" or
         * "the panel"), {1} the version the panel runs. At most 111 bytes */
        I18N_FORMAT(detail, dlen, TR("Stopped by {0} · you have {1}, nothing was changed"), by,
                    st->running);
        *glyph = UI_ICON_CIRCLE_X;
        *tone = ui_tone_gray();
        break;
    }
    case V_ROLLED: {
        const char *bad = st->last.version[0] != '\0' ? st->last.version : st->rolled_back;
        /* TR: Software update status after an update failed its self-test; {0} is that version.
         * At most 50 bytes */
        I18N_FORMAT(title, tlen, TR("Version {0} failed its self-test"), bad);
        /* TR: the line under it; {0} is the version running again. At most 80 bytes */
        I18N_FORMAT(detail, dlen, TR("Back on {0} by itself"), st->running);
        *glyph = UI_ICON_TRIANGLE_ALERT;
        *tone = ui_tone_amber();
        break;
    }
    case V_UPDATED:
        /* TR: Software update status after a successful update; {0} is the version. At most 50
         * bytes */
        I18N_FORMAT(title, tlen, TR("Updated to version {0}"), st->running);
        if (st->last.at != 0u && ui_clock_known()) {
            char when[32];
            ui_time_or_day_text((int64_t)st->last.at * 1000, when, sizeof(when));
            /* TR: the line under "Updated to version ..."; {0} is a time like 7:14 AM or a day
             * like Sep 19. At most 80 bytes */
            I18N_FORMAT(detail, dlen, TR("{0} · it passed its self-test"), when);
        } else {
            /* TR: the line under "Updated to version ..." when the time isn't known. At most 80
             * bytes */
            i18n_utf8_copy(detail, dlen, TR("It passed its self-test"));
        }
        *glyph = UI_ICON_CIRCLE_CHECK;
        *tone = ui_tone_green();
        break;
    default:
        if (st->withdrawn) {
            /* TR: Software update status when the publisher withdrew the version the panel runs;
             * {0} is that version. At most 50 bytes */
            I18N_FORMAT(title, tlen, TR("Version {0} was withdrawn"), st->running);
            if (st->available && st->critical) {
                /* TR: the line under "Version ... was withdrawn" when the offered version is an
                 * important fix; {0} is the offered version. At most 90 bytes */
                I18N_FORMAT(detail, dlen, TR("Its publisher took it back · {0} is an important fix"),
                            st->latest);
            } else if (st->available) {
                /* TR: the line under "Version ... was withdrawn" when a newer version is offered;
                 * {0} is the offered version. At most 90 bytes */
                I18N_FORMAT(detail, dlen, TR("Its publisher took it back · {0} is ready to install"),
                            st->latest);
            } else {
                /* TR: the line under "Version ... was withdrawn" when nothing newer is offered. At
                 * most 90 bytes */
                i18n_utf8_copy(detail, dlen, TR("Its publisher took it back"));
            }
            *glyph = UI_ICON_TRIANGLE_ALERT;
            *tone = ui_tone_amber();
        } else if (st->available) {
            /* TR: Software update status; {0} is a version number such as 1.4.2. At most 40
             * bytes */
            I18N_FORMAT(title, tlen, TR("Version {0} is ready to install"), st->latest);
            if (st->critical) {
                /* TR: the line under it when the offered version is an important fix; {0} is the
                 * installed version number. At most 90 bytes */
                I18N_FORMAT(detail, dlen, TR("An important fix · you have {0}"), st->running);
            } else {
                /* TR: the line under it; {0} is the installed version number. At most 90 bytes */
                I18N_FORMAT(detail, dlen, TR("You have {0} · the alarm keeps working while it downloads"),
                            st->running);
            }
            *glyph = UI_ICON_SPARKLES;
            *tone = ui_tone_violet();
        } else if (!st->has_source) {
            /* TR: Software update status when no update source is set. At most 63 bytes */
            i18n_utf8_copy(title, tlen, TR("Updates not set up"));
            /* TR: the line under it; {0} is the installed version number. At most 80 bytes */
            I18N_FORMAT(detail, dlen, TR("Version {0} · no update source"), st->running);
            *glyph = UI_ICON_CLOUD_OFF;
            *tone = ui_tone_gray();
        } else if (st->latest[0] != '\0' || st->checked != 0u) {
            /* TR: Software update status when the newest version is installed. At most 63
             * bytes */
            i18n_utf8_copy(title, tlen, TR("Up to date"));
            /* TR: the line under it; {0} is the installed version number. At most 80 bytes */
            I18N_FORMAT(detail, dlen, TR("Version {0}"), st->running);
            *glyph = UI_ICON_CIRCLE_CHECK;
            *tone = ui_tone_green();
        } else {
            /* TR: Software update status before the first check. At most 63 bytes */
            i18n_utf8_copy(title, tlen, TR("Not checked yet"));
            /* TR: the line under it; {0} is the installed version number. At most 80 bytes */
            I18N_FORMAT(detail, dlen, TR("Version {0}"), st->running);
            *glyph = UI_ICON_CIRCLE_HELP;
            *tone = ui_tone_gray();
        }
        break;
    }
}

/* When it last looked: "Checked 10:41 PM", or "Checked Sep 19" on another day; "" before the
 * first check, or while the panel doesn't know the time. */
static void checked_words(uint32_t checked, char *out, size_t len)
{
    out[0] = '\0';
    if (checked != 0u && ui_clock_known()) {
        char when[32];
        ui_time_or_day_text((int64_t)checked * 1000, when, sizeof(when));
        /* TR: Software update (About's row and Check again): when it last looked; {0} is a time
         * like 10:41 PM, or a day like Sep 19. At most 47 bytes */
        I18N_FORMAT(out, len, TR("Checked {0}"), when);
    }
}

/* About's row (BL-189, BL-188): to anyone, only "Up to date", "Update" (a version is offered),
 * "Updating" (a run: from Install to the end of the self-test) or "Rolled back". Once the installer
 * or the system master has given the code in this visit to Settings, a run shows its step and, where
 * it is known, the percent (OTA-022). The page behind it asks for the code. */
static lv_obj_t *s_about_row; /* the row of the open About page, NULL when none */
static uint8_t s_about_kind;  /* what it was built as (about_kind) */
static uint32_t s_about_polled;

static bool about_unlocked(void)
{
    return ui_settings_allowed(ACCESS_SYSTEM);
}

/* 0 plain, 1 offered, 2 rolled back, 3 a run for anyone, 4 a run with its step (after the code). */
static uint8_t about_kind(const ui_fw_status_t *st)
{
    const view_t v = view_of(st);
    if (is_run(v) || v == V_FILE) {
        return about_unlocked() ? 4u : 3u;
    }
    if (v == V_ROLLED) {
        return 2u;
    }
    return st->available ? 1u : 0u;
}

/* The run's step in words and the percent, for the unlocked row. */
static void about_words(const ui_fw_status_t *st, char *step, size_t slen, char *pct, size_t plen)
{
    char detail[112];
    const char *glyph;
    lv_color_t tone;
    int bar;
    status_words(st, step, slen, detail, sizeof(detail), &glyph, &tone, &bar);
    pct[0] = '\0';
    const view_t v = view_of(st);
    const int p = percent_of(st);
    if (p >= 0 && (v == V_DOWNLOAD || v == V_WIFI || v == V_FILE)) {
        pct_text(p, pct, plen);
    }
}

lv_obj_t *ui_update_about_row(lv_obj_t *body, ui_click_fn fn, void *ctx)
{
    ui_fw_status_t st;
    read_status(&st);
    const view_t v = view_of(&st);
    const bool running = is_run(v) || v == V_FILE;
    const bool rolled = v == V_ROLLED;
    const bool offered = !running && !rolled && st.available;
    const char *glyph = UI_ICON_REFRESH;
    lv_color_t tone = ui_tone_blue();
    const char *value;
    bool colored = true;
    char step[64] = "", pct[16] = "";
    if (running) {
        if (about_unlocked()) {
            about_words(&st, step, sizeof(step), pct, sizeof(pct));
            value = pct;
        } else {
            /* TR: Settings > About, Software update's value while an update is being installed. At
             * most 23 bytes */
            value = TR("Updating");
        }
        glyph = UI_ICON_DOWNLOAD;
    } else if (rolled) {
        /* TR: Settings > About, Software update's value after an update failed its self-test.
         * At most 23 bytes */
        value = TR("Rolled back");
        glyph = UI_ICON_TRIANGLE_ALERT;
        tone = ui_tone_amber();
    } else if (offered) {
        /* TR: Settings > About, Software update's value when an update is offered. At most 23
         * bytes */
        value = TR("Update");
        glyph = UI_ICON_SPARKLES;
        tone = ui_tone_violet();
    } else {
        /* TR: Settings > About, Software update's value when nothing is waiting. At most 23
         * bytes */
        value = TR("Up to date");
        glyph = UI_ICON_CIRCLE_CHECK;
        tone = ui_tone_green();
        colored = false;
    }
    const bool stepped = running && about_unlocked();
    lv_obj_t *row = ui_row_link(body, glyph, tone, TR("Software update"), stepped ? step : NULL, value,
                                UI_ICON_CHEVRON_RIGHT, fn, ctx);
    if (colored && !stepped) {
        lv_obj_set_style_text_color(ui_find(row, "value_label"), tone, 0);
    }
    s_about_row = row;
    s_about_kind = about_kind(&st);
    s_about_polled = lv_tick_get();
    return row;
}

/* About's row follows the run once a second: its step and percent in place; a different kind of
 * row (a run began or ended, the code was given) builds the page again. */
static void about_tick(void)
{
    if (s_about_row == NULL || lv_tick_elaps(s_about_polled) < POLL_MS) {
        return;
    }
    s_about_polled = lv_tick_get();
    ui_fw_status_t st;
    read_status(&st);
    if (about_kind(&st) != s_about_kind) {
        if (lv_subject_get_int(&subject_sheet) == SHEET_NONE && S.pad == PAD_NONE) {
            ui_page_rebuild();
        }
        return;
    }
    if (s_about_kind == 4u) {
        char step[64], pct[16];
        about_words(&st, step, sizeof(step), pct, sizeof(pct));
        ui_set_label(s_about_row, "detail_label", step);
        ui_set_label(s_about_row, "value_label", pct);
    }
}

static void show_status(void)
{
    if (s_row == NULL) {
        return;
    }
    char title[64];
    char detail[112];
    const char *glyph;
    lv_color_t tone;
    int bar;
    status_words(&s_shown, title, sizeof(title), detail, sizeof(detail), &glyph, &tone, &bar);
    ui_set_label(s_row, "title_label", title);
    ui_set_label(s_row, "detail_label", detail);
    lv_obj_t *dl = ui_find(s_row, "detail_label");
    if (detail[0] != '\0') {
        lv_obj_remove_flag(dl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dl, LV_OBJ_FLAG_HIDDEN);
    }
    ui_chip_tone(ui_find(s_row, "chip"), glyph, tone);
    lv_obj_t *level = ui_find(s_row, "level");
    if (level != NULL) {
        if (bar >= 0) {
            lv_obj_remove_flag(level, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(level, bar, LV_ANIM_OFF); /* the whole percent, once a second, no glide */
            lv_obj_set_style_bg_color(level, tone, LV_PART_INDICATOR);
        } else {
            lv_obj_add_flag(level, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_update_stop(void)
{
    s_row = NULL;
    s_about_row = NULL;
    s_asking = false;
    ui_fwsrv_stop();
}

/* Everything that changes what the page is made of (not only what its status row says). */
static bool page_changed(const ui_fw_status_t *a, const ui_fw_status_t *b)
{
    const view_t va = view_of(a);
    return va != view_of(b) || a->available != b->available || a->withdrawn != b->withdrawn ||
           a->critical != b->critical || a->has_source != b->has_source ||
           a->release != b->release || (a->size != 0u) != (b->size != 0u) || a->checked != b->checked ||
           strcmp(a->rolled_back, b->rolled_back) != 0 ||
           (va == V_WAIT && why_kind(a->reason) != why_kind(b->reason)) ||
           (va == V_STOPPED && strcmp(a->last.by, b->last.by) != 0);
}

void ui_update_tick(void)
{
    about_tick();
    fwsrv_tick();
    if (s_row == NULL || lv_tick_elaps(s_polled) < POLL_MS) {
        return;
    }
    s_polled = lv_tick_get();
    ui_fw_status_t st;
    read_status(&st);
    if (s_asking) {
        if (lv_subject_get_int(&subject_sheet) == SHEET_NONE) {
            s_asking = false; /* answered or closed elsewhere */
        } else if (!stoppable(view_of(&st), &st)) {
            ui_sheet_close(); /* the run ended or changed under the question (UX-033) */
            s_asking = false;
        }
    }
    const bool buttons = page_changed(&st, &s_shown);
    if (buttons && lv_subject_get_int(&subject_sheet) == SHEET_NONE && S.pad == PAD_NONE) {
        ui_page_rebuild(); /* other buttons now */
        return;
    }
    if (buttons || st.percent != s_shown.percent || st.bytes != s_shown.bytes ||
        st.size != s_shown.size || strcmp(st.reason, s_shown.reason) != 0 ||
        strcmp(st.latest, s_shown.latest) != 0 || strcmp(st.target, s_shown.target) != 0 ||
        st.stage != s_shown.stage || st.phase != s_shown.phase) {
        s_shown = st;
        show_status();
    }
}

/* ---- actions -------------------------------------------------------------------------------- */

static void install_confirmed(void)
{
    if (S.be.fw_install == NULL || !S.be.fw_install(S.be.ctx, S.auth_user)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("An update is already running"));
        return;
    }
    s_polled = lv_tick_get() - POLL_MS; /* the page shows the download at once: no toast */
}

static void on_install(void *ctx)
{
    (void)ctx;
    char title[64];
    /* TR: the question before installing an update; {0} is the version number. At most 32
     * bytes */
    I18N_FORMAT(title, sizeof(title), TR("Install version {0}?"), s_shown.latest);
    /* TR: the question before installing an update (the sheet's note). At most 190 bytes */
    ui_confirm_primary(title,
                       TR("The panel downloads it now and restarts once everything is disarmed. It "
                          "keeps protecting while it downloads and waits, and you can stop the update "
                          "until it restarts."),
                       TR("Install"), install_confirmed);
}

static void stop_confirmed(void)
{
    s_asking = false;
    uint8_t r = UI_FW_STOP_NOTHING;
    if (S.be.fw_stop != NULL) {
        r = S.be.fw_stop(S.be.ctx, S.auth_user);
    }
    if (r == UI_FW_STOP_NOTHING) {
        ui_toast(TR("It isn't running any more"));
    } else if (r == UI_FW_STOP_NOT_STOPPABLE) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("This update can't be stopped"));
    }
    s_polled = lv_tick_get() - POLL_MS; /* the page says "Stopping" at once */
}

static void stop_kept(void)
{
    s_asking = false;
}

static void on_stop(void *ctx)
{
    (void)ctx;
    char note[192];
    /* TR: the question before stopping an update; {0} is the version the panel keeps, {1} the one
     * on offer. At most 190 bytes */
    I18N_FORMAT(note, sizeof(note),
                TR("The panel keeps version {0}. You can install {1} again later; it downloads it again."),
                s_shown.running, s_shown.target[0] != '\0' ? s_shown.target : s_shown.latest);
    ui_confirm_choice(TR("Stop the update?"), note, TR("Stop it"), stop_confirmed,
                      TR("Keep updating"), stop_kept);
    s_asking = true;
}

static void on_check(void *ctx)
{
    (void)ctx;
    if (S.be.fw_check != NULL) {
        S.be.fw_check(S.be.ctx);
        s_polled = lv_tick_get() - POLL_MS; /* the status row and Check again say it: no toast */
    }
}

/* ---- the block ------------------------------------------------------------------------------ */

static void steps(lv_obj_t *body, const ui_fw_status_t *st, unsigned now)
{
    /* TR: Software update, the heading over the steps of an update. At most 47 bytes */
    ui_section(body, TR("Steps"));
    char size[32] = "";
    if (st->size != 0u) {
        char mb[16];
        i18n_number(mb, sizeof(mb), ui_region(), (int64_t)(((uint64_t)st->size * 10u + (1u << 19)) >> 20), 1u);
        /* TR: the download's size in megabytes, a step's line in Software update; {0} is the
         * number. At most 31 bytes */
        I18N_FORMAT(size, sizeof(size), TR_C("data size", "{0} MB"), mb);
    }
    char selftest[112];
    if (st->previous[0] != '\0') {
        /* TR: Software update, the last step's line; {0} is the version it goes back to. At most
         * 111 bytes */
        I18N_FORMAT(selftest, sizeof(selftest),
                    TR("Usually a minute, at most three · restarts with {0} if it fails"), st->previous);
    } else {
        /* TR: Software update, the last step's line. At most 111 bytes */
        i18n_utf8_copy(selftest, sizeof(selftest),
                       TR("Usually a minute, at most three · goes back if it fails"));
    }
    const char *const titles[5] = {
        /* TR: Software update, a step of the update. At most 47 bytes */
        TR("Download"), TR("Check the download"), TR("Wait until everything is disarmed"),
        TR("Restart with the new version"), TR("Self-test")};
    const char *const details[5] = {
        size,
        /* TR: Software update, the line under a step. At most 79 bytes */
        TR("Its signature and checksum"), TR("Right away when it already is"),
        TR("About a minute; the self-test starts at once"), selftest};
    for (unsigned i = 0; i < 5u; i++) {
        ui_row_link(body, i < now ? UI_ICON_CIRCLE_CHECK : i == now ? UI_ICON_CIRCLE_DOT : UI_ICON_CIRCLE,
                    i < now ? ui_tone_green() : i == now ? ui_tone_blue() : ui_tone_gray(), titles[i],
                    details[i][0] != '\0' ? details[i] : NULL, NULL, NULL, NULL, NULL);
    }
}

void ui_update_build(lv_obj_t *body)
{
    s_row = NULL;
    s_asking = false;
    if (S.be.fw_status == NULL) {
        return;
    }
    read_status(&s_shown);
    s_polled = lv_tick_get();
    const view_t v = view_of(&s_shown);
    s_row = ui_row_link(body, UI_ICON_REFRESH, ui_tone_gray(), " ", " ", NULL, NULL, NULL, NULL);
    show_status();
    const bool stop = stoppable(v, &s_shown);
    if (stop) {
        ui_row_action(body, UI_ICON_CIRCLE_X, TR("Stop the update"), ui_tone_red(), on_stop, NULL);
    }
    unsigned step = 0;
    switch (v) {
    case V_DOWNLOAD:
        /* TR: Software update, under Stop while the update downloads. At most 111 bytes */
        ui_paragraph(body, TR("The alarm keeps working while it downloads. You can stop the update until the panel restarts."),
                     ui_tone_gray());
        step = 0;
        break;
    case V_VERIFY:
        /* TR: Software update, under Stop while the download is checked. At most 111 bytes */
        ui_paragraph(body, TR("The alarm keeps working while it checks. You can stop the update until the panel restarts."),
                     ui_tone_gray());
        step = 1;
        break;
    case V_WAIT:
        ui_paragraph(body, wait_paragraph(why_kind(s_shown.reason)), ui_tone_gray());
        step = 2;
        break;
    case V_INSTALL: step = 3; break;
    case V_STOPPING: step = s_step; break;
    case V_SELFTEST:
        if (s_shown.previous[0] != '\0') {
            char text[160];
            /* TR: Software update, during the self-test; {0} is the version it goes back to. At
             * most 111 bytes */
            I18N_FORMAT(text, sizeof(text),
                        TR("If its self-test fails, the panel restarts with {0} by itself, armed or not. The screen is back in about a minute."),
                        s_shown.previous);
            ui_paragraph(body, text, ui_tone_gray());
        } else {
            /* TR: Software update, during the self-test. At most 127 bytes */
            ui_paragraph(body, TR("If its self-test fails, the panel goes back to the old version by itself, armed or not. The screen is back in about a minute."),
                         ui_tone_gray());
        }
        step = 4;
        break;
    case V_WIFI:
        if (s_shown.phase != UI_FW_WAITING) {
            /* TR: Software update, while the Wi-Fi module is updated. At most 111 bytes */
            ui_paragraph(body, TR("The alarm keeps working while it sends the update to the module."),
                         ui_tone_gray());
        }
        break;
    default: break;
    }
    if (v == V_DOWNLOAD || v == V_VERIFY || v == V_WAIT || v == V_INSTALL) {
        s_step = (uint8_t)step;
    }
    if (v >= V_DOWNLOAD && v <= V_STOPPING && v != V_BACK) {
        steps(body, &s_shown, step);
    } else if (v == V_SELFTEST) {
        steps(body, &s_shown, step);
    } else if (v != V_BACK) {
        s_step = 0;
    }
    if (s_shown.available && !busy(&s_shown) && S.be.fw_install != NULL && v != V_UPDATED) {
        char text[64];
        /* TR: a button in Software update; {0} is the version number. At most 32 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Install version {0}"), s_shown.latest);
        ui_row_action(body, UI_ICON_DOWNLOAD, text, ui_tone_blue(), on_install, NULL);
        if (v == V_PLAIN || v == V_ROLLED || v == V_STOPPED) {
            ui_paragraph(body, TR("The panel restarts once everything is disarmed. It takes about a minute."),
                         ui_tone_gray());
        }
    }
    if (s_shown.has_source && S.be.fw_check != NULL && s_shown.stage == UI_FW_STAGE_NONE &&
        !s_shown.confirming) {
        const bool checking = v == V_CHECKING;
        char checked[48];
        checked_words(checking ? 0u : s_shown.checked, checked, sizeof(checked));
        /* TR: Software update, the row that looks for a new version. At most 47 bytes */
        ui_row_link(body, UI_ICON_REFRESH, ui_tone_blue(), TR("Check again"),
                    /* TR: Software update, Check again's line while it looks. At most 47 bytes */
                    checking ? TR("Checking now") : (checked[0] != '\0' ? checked : NULL), NULL, NULL,
                    checking ? NULL : on_check, NULL);
    }
}

/* ---- Installer > Updates: the update server and the channel (BL-186) ------------------------ */

#define OFFICIAL_DOMAIN "roboalarms.com" /* this build's own server (RoboAlarms' builds) */

typedef enum {
    SV_IDLE = 0, /* the server in use, and the way to change it */
    SV_CHECKING, /* a typed domain is being asked */
    SV_REFUSED,  /* the check ended in a refusal (s_sv_refusal) */
    SV_REVIEW,   /* a good check waits for the installer's answer (the warning, or the question) */
} sv_state_t;

static sv_state_t s_sv;
static uint8_t s_sv_refusal;     /* ui_fw_srv_result_t of the refusal */
static char s_sv_domain[96];     /* the domain being checked, or refused */
static ui_fw_srv_check_t s_sv_check;
static bool s_sv_live;           /* a check exists on the panel: leaving the page forgets it */
static bool s_sv_warn;           /* the warning page was opened: coming back starts again */
static uint32_t s_sv_polled;
static bool s_menu_held;         /* the Updates rows were drawn dimmed */
static uint32_t s_menu_polled;

static bool update_runs(void)
{
    ui_fw_status_t st;
    read_status(&st);
    return st.stage != UI_FW_STAGE_NONE || st.confirming;
}

static void sv_read(ui_fw_server_t *srv)
{
    memset(srv, 0, sizeof(*srv));
    if (S.be.fw_server != NULL) {
        S.be.fw_server(S.be.ctx, srv);
    }
}

static bool factory_installer_code(void)
{
    return (S.snap.default_codes & ALARM_DEFAULT_CODE_INSTALLER) != 0u;
}

static bool on_fw_page(void)
{
    return S.depth > 0u && lv_subject_get_int(&subject_view) == VIEW_SETTINGS &&
           (S.pages[S.depth - 1u].id == PAGE_FW_SERVER || S.pages[S.depth - 1u].id == PAGE_FW_WARNING);
}

static void sv_forget(void)
{
    if (s_sv_live && S.be.fw_server_check_forget != NULL) {
        S.be.fw_server_check_forget(S.be.ctx);
    }
    s_sv_live = false;
    s_sv = SV_IDLE;
    s_sv_warn = false;
    memset(&s_sv_check, 0, sizeof(s_sv_check));
}

/* A page is dropped (ui_rows_reset): leaving the update server's pages forgets the check. */
void ui_fwsrv_stop(void)
{
    if (!on_fw_page()) {
        sv_forget();
    }
}

/* ---- the channel ---- */

typedef struct {
    const char *id;
    const char *name;
    const char *detail;
    const char *glyph;
} channel_t;

static const channel_t CHANNELS[4] = {
    /* TR: Installer > Updates > Update channel: the channel's name. At most 23 bytes */
    {"stable", TR_NOOP("Stable"),
     /* TR: the channel list, under Stable. At most 63 bytes */
     TR_NOOP("Tested the longest. Right for most homes"), UI_ICON_SHIELD_CHECK},
    /* TR: Installer > Updates > Update channel: the channel's name. At most 23 bytes */
    {"beta", TR_NOOP("Beta"),
     /* TR: the channel list, under Beta. At most 63 bytes */
     TR_NOOP("Newer releases, tried by a few people first"), UI_ICON_FLASK_CONICAL},
    /* TR: Installer > Updates > Update channel: the channel's name. At most 23 bytes */
    {"alpha", TR_NOOP("Alpha"),
     /* TR: the channel list, under Alpha. At most 63 bytes */
     TR_NOOP("The newest releases, still being tested"), UI_ICON_FLASK_CONICAL},
    /* TR: Installer > Updates > Update channel: the channel's name. At most 23 bytes */
    {"dev", TR_NOOP("Dev"),
     /* TR: the channel list, under Dev. At most 63 bytes */
     TR_NOOP("Special builds for developers"), UI_ICON_CODE_XML},
};

static int channel_index(const char *id)
{
    for (int i = 0; i < 4; i++) {
        if (strcmp(CHANNELS[i].id, id) == 0) {
            return i;
        }
    }
    return 0;
}

static void channel_chosen(int32_t index)
{
    ui_fw_server_t srv;
    sv_read(&srv);
    if (index < 0 || index >= 4 || index == channel_index(srv.channel) || S.be.fw_set_channel == NULL) {
        return;
    }
    if (!S.be.fw_set_channel(S.be.ctx, CHANNELS[index].id, S.auth_user)) {
        ui_beep(UI_BEEP_ERROR);
        /* TR: a toast when the update server or the channel can't change because an update runs.
         * At most 63 bytes */
        ui_toast(TR("Not while an update runs"));
    }
    ui_page_rebuild(); /* the row's value; the panel checks at once */
}

static void on_channel(void *ctx)
{
    (void)ctx;
    ui_fw_server_t srv;
    sv_read(&srv);
    static ui_choice_t choices[4] UI_SLOW_BSS;
    for (int i = 0; i < 4; i++) {
        choices[i].glyph = CHANNELS[i].glyph;
        choices[i].tone = i == 0 ? ui_tone_green() : i == 3 ? ui_tone_gray() : i == 1 ? ui_tone_blue() : ui_tone_violet();
        choices[i].title = TR(CHANNELS[i].name);
        choices[i].detail = TR(CHANNELS[i].detail);
    }
    /* TR: Installer > Updates, the channel list's title. At most 47 bytes */
    ui_pick_sheet(TR("Update channel"),
                  /* TR: the channel list's note. At most 127 bytes */
                  TR("Which releases this panel is offered"), choices, 4, channel_index(srv.channel),
                  channel_chosen);
}

/* ---- the Installer menu's group ---- */

static void on_server(void *ctx)
{
    (void)ctx;
    sv_forget();
    ui_page_open(PAGE_FW_SERVER, 0);
}

void ui_fwsrv_menu(lv_obj_t *body)
{
    if (S.be.fw_server == NULL) {
        return;
    }
    ui_fw_server_t srv;
    sv_read(&srv);
    s_menu_held = update_runs();
    s_menu_polled = lv_tick_get();
    /* TR: Installer menu, the heading over the update server and the channel. At most 47 bytes */
    ui_section(body, TR("Updates"));
    /* TR: Installer > Updates, the update server row. At most 47 bytes */
    lv_obj_t *row = ui_row_link(body, UI_ICON_SERVER, srv.official ? ui_tone_green() : ui_tone_amber(),
                                TR("Update server"), NULL, srv.domain, UI_ICON_CHEVRON_RIGHT, on_server, NULL);
    if (s_menu_held) {
        /* TR: Installer > Updates, the line under a row that can't change while an update runs.
         * At most 47 bytes */
        ui_row_set_detail(row, TR("Not while an update runs"), false, ui_tone_gray());
        ui_row_disable(row);
    } else if (srv.official) {
        /* TR: Installer > Updates, under Update server when it is this build's own. At most 47 bytes */
        ui_row_set_detail(row, TR("Where updates come from"), false, ui_tone_gray());
    } else {
        /* TR: Installer > Updates, under Update server when it isn't this build's own. At most 47
         * bytes */
        ui_row_set_detail(row, TR("Not the RoboAlarms server"), true, ui_tone_amber());
    }
    lv_obj_t *ch = ui_row_link(body, UI_ICON_LAYERS, ui_tone_blue(), TR("Update channel"), NULL,
                               TR(CHANNELS[channel_index(srv.channel)].name), UI_ICON_CHEVRON_RIGHT,
                               on_channel, NULL);
    if (s_menu_held) {
        ui_row_set_detail(ch, TR("Not while an update runs"), false, ui_tone_gray());
        ui_row_disable(ch);
    } else {
        /* TR: Installer > Updates, under Update channel. At most 63 bytes */
        ui_row_set_detail(ch, TR("Which releases this panel is offered"), false, ui_tone_gray());
    }
}

/* ---- the server page ---- */

static void commit_now(void)
{
    uint8_t r = UI_FW_COMMIT_STALE;
    if (S.be.fw_server_commit != NULL) {
        r = S.be.fw_server_commit(S.be.ctx, s_sv_check.domain, s_sv_check.key_sha256,
                                  S.auth_user);
    }
    const bool warning = S.depth > 0u && S.pages[S.depth - 1u].id == PAGE_FW_WARNING;
    if (r == UI_FW_COMMIT_OK) {
        sv_forget();
    } else {
        ui_beep(UI_BEEP_ERROR);
        if (r == UI_FW_COMMIT_FACTORY_CODE) {
            /* TR: a toast: the update server can't change while the installer code is the factory
             * one. At most 63 bytes */
            ui_toast(TR("Change the installer code first"));
        } else if (r == UI_FW_COMMIT_RUNNING) {
            ui_toast(TR("Not while an update runs"));
        } else {
            /* TR: a toast: the check of the new update server is no longer valid. At most 63
             * bytes */
            ui_toast(TR("That check is out of date: check again"));
        }
        sv_forget();
    }
    if (warning) {
        ui_page_back(); /* the server page, with what is in use now */
    } else {
        ui_page_rebuild();
    }
}

static void begin_check(const char *typed)
{
    sv_forget();
    s_sv_domain[0] = '\0';
    if (S.be.fw_server_check == NULL || !S.be.fw_server_check(S.be.ctx, typed)) {
        ui_fw_srv_check_t c;
        memset(&c, 0, sizeof(c));
        if (S.be.fw_server_check_get != NULL) {
            S.be.fw_server_check_get(S.be.ctx, &c);
        }
        ui_beep(UI_BEEP_ERROR);
        if (c.result == UI_FW_SRV_BAD_DOMAIN) {
            /* TR: a toast when what was typed isn't a domain, or nothing was typed. At most 63
             * bytes */
            ui_toast(TR("Type a domain, like roboalarms.com"));
        } else {
            ui_toast(TR("Not while an update runs"));
        }
        return;
    }
    s_sv_live = true;
    s_sv = SV_CHECKING;
    s_sv_polled = lv_tick_get();
    ui_fw_srv_check_t c;
    memset(&c, 0, sizeof(c));
    if (S.be.fw_server_check_get != NULL) {
        S.be.fw_server_check_get(S.be.ctx, &c);
    }
    snprintf(s_sv_domain, sizeof(s_sv_domain), "%s", c.domain[0] != '\0' ? c.domain : typed);
    ui_page_rebuild();
}

static void typed_done(const char *text)
{
    char d[96];
    snprintf(d, sizeof(d), "%s", text);
    size_t a = 0, n = strlen(d);
    while (a < n && d[a] == ' ') {
        a++;
    }
    while (n > a && d[n - 1u] == ' ') {
        n--;
    }
    memmove(d, d + a, n - a);
    d[n - a] = '\0';
    if (d[0] == '\0') {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Type a domain, like roboalarms.com"));
        return;
    }
    begin_check(d);
}

static void on_change(void *ctx)
{
    (void)ctx;
    /* TR: Installer > Update server, the text sheet's title. At most 47 bytes */
    ui_text_sheet_kind(TR("Update server"),
                       /* TR: the text sheet's note: what to type. At most 127 bytes */
                       TR("The domain only, like roboalarms.com"), "", UI_TEXT_PLAIN, 95, typed_done);
    /* TR: the text sheet's button: asks the server before anything is kept. At most 23 bytes */
    ui_text_sheet_ok(TR("Check"));
}

static void on_go_back(void *ctx)
{
    (void)ctx;
    begin_check(OFFICIAL_DOMAIN);
}

/* The page's rows: the server (or what the check says), the key, the way to change it. */
static void server_header(lv_obj_t *body, const ui_fw_server_t *srv)
{
    char text[112];
    lv_obj_t *row;
    switch (s_sv) {
    case SV_CHECKING:
        /* TR: Installer > Update server, while the typed server is asked; {0} is its domain. At
         * most 63 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Checking {0}"), s_sv_domain);
        ui_row_link(body, UI_ICON_REFRESH, ui_tone_blue(), text,
                    /* TR: the line under "Checking ...". At most 79 bytes */
                    TR("Asking for its signing key and for firmware for this panel"), NULL, NULL, NULL, NULL);
        return;
    case SV_REFUSED: {
        const char *detail;
        if (s_sv_refusal == UI_FW_SRV_NOT_A_SERVER) {
            /* TR: Installer > Update server, a refusal; {0} is the domain. At most 63 bytes */
            I18N_FORMAT(text, sizeof(text), TR("{0} isn't an update server"), s_sv_domain);
            /* TR: the line under that refusal. At most 79 bytes */
            detail = TR("It didn't answer like one. Check the domain");
        } else if (s_sv_refusal == UI_FW_SRV_NO_FIRMWARE) {
            /* TR: Installer > Update server, a refusal; {0} is the domain. At most 63 bytes */
            I18N_FORMAT(text, sizeof(text), TR("{0} has no firmware for this panel"), s_sv_domain);
            /* TR: the line under that refusal. At most 79 bytes */
            detail = TR("It doesn't serve this model of panel");
        } else {
            /* TR: Installer > Update server, a refusal; {0} is the domain. At most 63 bytes */
            I18N_FORMAT(text, sizeof(text), TR("{0} can't be reached"), s_sv_domain);
            /* TR: the line under that refusal. At most 79 bytes */
            detail = TR("Check the domain and that the panel is online");
        }
        ui_row_link(body, UI_ICON_CIRCLE_X, ui_tone_red(), text, detail, NULL, NULL, NULL, NULL);
        return;
    }
    default: break;
    }
    row = ui_row_link(body, UI_ICON_SERVER, srv->official ? ui_tone_green() : ui_tone_amber(), srv->domain,
                      /* TR: Installer > Update server, under the domain when it is this build's own.
                       * At most 63 bytes */
                      srv->official ? TR("The RoboAlarms update server") : TR("Not the RoboAlarms server"),
                      NULL, NULL, NULL, NULL);
    if (!srv->official) {
        ui_row_set_detail(row, TR("Not the RoboAlarms server"), true, ui_tone_amber());
    }
}

static void build_server(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    if (s_sv_warn) {
        sv_forget(); /* back from the warning: nothing changed, start again */
    }
    /* TR: Installer > Update server, the page title. At most 47 bytes */
    i18n_utf8_copy(title, tlen, TR("Update server"));
    /* TR: the line under the title. At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("Where this panel gets its updates"));
    ui_fw_server_t srv;
    sv_read(&srv);
    server_header(body, &srv);
    if (srv.key_name[0] != '\0' || srv.fingerprint[0] != '\0') {
        /* TR: Installer > Update server, the heading over the key's name and fingerprint. At most
         * 47 bytes */
        ui_section(body, TR("Signing key"));
        ui_row_value(body, srv.key_name[0] != '\0' ? srv.key_name : TR("Not known yet"), srv.fingerprint);
        if (s_sv != SV_CHECKING) {
            ui_paragraph(body,
                         srv.official
                             /* TR: Installer > Update server, under the key. At most 127 bytes */
                             ? TR("Every update must be signed with this key. The panel refuses anything else, wherever it comes from.")
                             /* TR: the same, for another project's server. At most 127 bytes */
                             : TR("Every update must be signed with this key or with the key of the version it runs."),
                         ui_tone_gray());
        }
    }
    if (s_sv == SV_CHECKING) {
        return;
    }
    ui_section(body, "");
    /* TR: Installer > Update server, the row that types a new domain. At most 47 bytes */
    lv_obj_t *change = ui_row_link(body, UI_ICON_PENCIL, factory_installer_code() ? ui_tone_gray() : ui_tone_blue(),
                                   TR("Change the update server"), NULL, NULL, UI_ICON_CHEVRON_RIGHT, on_change, NULL);
    if (factory_installer_code()) {
        /* TR: the line under that row while the installer code is the factory one. At most 79 bytes */
        ui_row_set_detail(change, TR("Change the installer code first: the factory one is public"), false,
                          ui_tone_gray());
        ui_row_disable(change);
    } else {
        /* TR: the line under that row. At most 79 bytes */
        ui_row_set_detail(change, TR("For your own builds or another project's"), false, ui_tone_gray());
    }
    if (!srv.official) {
        char text[96];
        /* TR: Installer > Update server, the button that goes back to this build's own server; {0}
         * is its domain. At most 63 bytes */
        I18N_FORMAT(text, sizeof(text), TR("Go back to {0}"), NOTR(OFFICIAL_DOMAIN));
        ui_row_action(body, UI_ICON_ROTATE_CCW, text, ui_tone_blue(), on_go_back, NULL);
    }
}

/* ---- the warning before a server whose key isn't trusted ---- */

static void use_it(void *ctx)
{
    (void)ctx;
    commit_now();
}

static void build_warning(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    /* TR: Installer > Update server > the warning, the page title; {0} is the domain. At most 47
     * bytes */
    I18N_FORMAT(title, tlen, TR("Use {0} for updates?"), s_sv_check.domain);
    if (strcmp(s_sv_check.domain, OFFICIAL_DOMAIN) != 0) {
        /* TR: the warning's line under the title. At most 127 bytes */
        i18n_utf8_copy(note, nlen, TR("Not the RoboAlarms update server"));
    } else {
        /* TR: the warning's line under the title when going back to this build's own server whose
         * key the panel doesn't trust now. At most 127 bytes */
        i18n_utf8_copy(note, nlen, TR("Its signing key is new to this panel"));
    }
    char text[112];
    /* TR: the warning's first row; {0} is the domain. At most 63 bytes */
    I18N_FORMAT(text, sizeof(text), TR("{0} answered"), s_sv_check.domain);
    ui_row_link(body, UI_ICON_SHIELD_ALERT, ui_tone_amber(), text,
                /* TR: the line under it. At most 79 bytes */
                TR("It has firmware for this panel"), NULL, NULL, NULL, NULL);
    /* TR: the warning's paragraph. At most 191 bytes */
    ui_paragraph(body,
                 TR("Whoever runs this server can install firmware that changes everything this panel does, its alarms too. Continue only if you trust them."),
                 ui_tone_amber());
    /* TR: the warning's heading over the key. At most 47 bytes */
    ui_section(body, TR("Its signing key"));
    ui_row_value(body, s_sv_check.key_name, s_sv_check.fingerprint);
    /* TR: the warning's paragraph under the key. At most 191 bytes */
    ui_paragraph(body,
                 TR("Check that this matches the key its owner publishes. From now on the panel also accepts updates signed with it."),
                 ui_tone_gray());
    /* TR: the hold button that keeps the new server. At most 47 bytes */
    ui_row_hold(body, TR("Hold to use this server"), use_it, NULL);
}

bool ui_fwsrv_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    if (id == PAGE_FW_SERVER) {
        build_server(body, title, tlen, note, nlen);
        return true;
    }
    if (id == PAGE_FW_WARNING) {
        build_warning(body, title, tlen, note, nlen);
        return true;
    }
    return false;
}

/* A good check: the warning (a key the panel doesn't trust) or the usual question. */
static void review_confirmed(void)
{
    commit_now();
}

static void sv_finished(const ui_fw_srv_check_t *c)
{
    s_sv_check = *c;
    if (c->result != UI_FW_SRV_OK) {
        s_sv = c->result == UI_FW_SRV_NONE ? SV_IDLE : SV_REFUSED;
        s_sv_refusal = c->result;
        s_sv_live = c->result != UI_FW_SRV_NONE;
        ui_beep(UI_BEEP_ERROR);
        ui_page_rebuild();
        return;
    }
    s_sv = SV_REVIEW;
    if (!c->trusted) {
        s_sv_warn = true;
        ui_page_open(PAGE_FW_WARNING, 0);
        return;
    }
    char title[64], text[192];
    /* TR: the question before keeping a checked update server whose key the panel trusts; {0} is
     * the domain. At most 47 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Use {0} for updates?"), c->domain);
    /* TR: its note; {0} is the key's name. At most 190 bytes */
    I18N_FORMAT(text, sizeof(text),
                TR("It is signed with {0}, a key this panel already trusts. The panel checks it for updates at once."),
                c->key_name);
    ui_confirm_primary(title, text, TR("Use it"), review_confirmed);
}

/* Once a second: the check's answer on the server page; the Installer menu's rows dim and wake with
 * a run. */
static void fwsrv_tick(void)
{
    if (!on_fw_page()) {
        if (S.depth > 0u && S.pages[S.depth - 1u].id == PAGE_INSTALLER && S.program && S.be.fw_server != NULL &&
            lv_subject_get_int(&subject_view) == VIEW_SETTINGS && s_menu_polled != 0u &&
            lv_tick_elaps(s_menu_polled) >= POLL_MS) {
            s_menu_polled = lv_tick_get();
            if (update_runs() != s_menu_held && lv_subject_get_int(&subject_sheet) == SHEET_NONE &&
                S.pad == PAD_NONE) {
                ui_page_rebuild();
            }
        }
        return;
    }
    if (S.pages[S.depth - 1u].id != PAGE_FW_SERVER || s_sv != SV_CHECKING ||
        lv_tick_elaps(s_sv_polled) < POLL_MS) {
        return;
    }
    s_sv_polled = lv_tick_get();
    ui_fw_srv_check_t c;
    memset(&c, 0, sizeof(c));
    if (S.be.fw_server_check_get != NULL) {
        S.be.fw_server_check_get(S.be.ctx, &c);
    }
    if (c.result == UI_FW_SRV_CHECKING) {
        return;
    }
    snprintf(s_sv_domain, sizeof(s_sv_domain), "%s", c.domain[0] != '\0' ? c.domain : s_sv_domain);
    sv_finished(&c);
}
