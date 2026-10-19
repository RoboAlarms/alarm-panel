/*
 * link_fw.c - see link_fw.h.
 */
#include "link_fw.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_proto/proto_fw.h"
#include "panel_link/plink_crypto.h"

#define FW_SERVER        "https://updates.example.org" /* the master's update server */
#define FW_CHANNEL       "stable"
#define FW_KEY_BYTE      0x4Bu  /* every PC image is signed with this key (its digest's bytes) */
#define FW_KP_SERVER     "https://keypads.example.net" /* a new keypad's own, until its master's */
#define FW_BPS_STEP      30000u /* bytes a download moves on in ... */
#define FW_STEP_MS       100u   /* ... this long: 300 KB/s */
#define FW_DOWN_MS       3000u  /* a restart: off this long */
#define FW_KP_GOES_MS    300u   /* a keypad's FW_STATE INSTALLING goes before it is off */
#define FW_PLAN_MS       1000u  /* the master's planner once a second ... */
#define FW_PLAN_FAST_MS  100u   /* ... and this often while its restart waits for the echoes */
#define FW_INSTALL_WAIT  5000u  /* the INSTALLs' echoes waited for at most this long */
#define FW_COPY_MS       2000u  /* a copy for another model: downloaded in this long */
#define MFW_TEST_MS      15000u /* the master's self-test: at least this long, ... */
#define MFW_BACK_MS      10000u /* ... a keypad back this long (LINK-041), ... */
#define MFW_TEST_MAX_MS  90000u /* ... or it goes back after this without one */
#define MFW_BAD_MS       20000u /* a self-test made to fail fails this long on */
#define KFW_TEST_MS      10000u /* a keypad's self-test: its link up this long */
#define KFW_ASK_MS       1000u  /* its gate's second ask */
#define KFW_FAILED_TRIAL 0x20u  /* the health bit a failed trial names (PANEL_OTA_OK_LINK) */
#define KFW_FAILED_TEST  0x04u  /* ... and a failed self-test (PANEL_OTA_OK_UI) */
#define SAY_PROGRESS_MS  1000u  /* a percent said at most this often */

static void say(const link_io_t *io, const char *fmt, ...) LINK_PRINTF(2, 3);
static void say(const link_io_t *io, const char *fmt, ...)
{
    if (io->say == NULL) {
        return;
    }
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    io->say(io->say_ctx, line);
}

static void put(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

#define PUT(dst, src) put((dst), sizeof(dst), (src))

static bool zeros(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* ---- images ---------------------------------------------------------------------------------- */

static uint32_t fnv(const char *a, const char *b)
{
    uint32_t h = 2166136261u;
    for (const char *s = a; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    h = (h ^ 0x2Fu) * 16777619u;
    for (const char *s = b; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

void link_img_free(link_img_t *img)
{
    free(img->bytes);
    memset(img, 0, sizeof(*img));
}

bool link_img_make(link_img_t *img, const char *device, const char *version)
{
    link_img_free(img);
    const uint32_t seed = fnv(device, version);
    const uint32_t size = 900000u + seed % 200000u;
    uint8_t *b = malloc(size);
    if (b == NULL) {
        return false;
    }
    uint32_t x = seed | 1u;
    for (uint32_t i = 0; i < size; i++) {
        x = x * 1664525u + 1013904223u;
        b[i] = (uint8_t)(x >> 24);
    }
    PUT(img->device, device);
    PUT(img->version, version);
    img->size = size;
    img->bytes = b;
    const plink_iov_t part = {b, size};
    plink_sha256(&part, 1u, img->sha);
    return true;
}

/* An image as the planner sees it. */
static void image_of(plink_fw_image_t *out, const link_img_t *im, uint8_t src)
{
    memset(out, 0, sizeof(*out));
    if (im->bytes == NULL) {
        return;
    }
    out->src = src;
    PUT(out->version, im->version);
    PUT(out->device, im->device);
    PUT(out->project, LINK_FW_PROJECT);
    out->chip_min = 0u;
    out->chip_max = 0xFFFFu;
    out->size = im->size;
    memcpy(out->sha256, im->sha, sizeof(out->sha256));
}

static const char *srv_word(uint8_t srv)
{
    return srv == LINK_SRV_OFF ? "can't be reached" : srv == LINK_SRV_REFUSE ? "refuses" : "answers";
}

static const char *why_word(uint8_t why)
{
    return why == 0u ? "-" : proto_fw_why_name(why);
}

/* ---- the master ------------------------------------------------------------------------------ */

void link_mfw_setup(link_mfw_t *f, const char *name, const char *id, const char *device,
                    const char *version)
{
    memset(f, 0, sizeof(*f));
    PUT(f->name, name != NULL ? name : "PC master");
    PUT(f->id, id != NULL ? id : "0ac5e1");
    PUT(f->device, device != NULL ? device : LINK_FW_DEVICE);
    PUT(f->running, version);
    f->confirmed = true;
    f->server = LINK_SRV_ON;
    (void)link_img_make(&f->run, f->device, f->running);
}

static bool updating(const link_mfw_t *f)
{
    return f->getting[0] != '\0' || f->spare[0] != '\0' || !f->confirmed;
}

void link_mfw_boot(link_master_t *v, uint64_t now_ms)
{
    link_mfw_t *f = v->io.mfw;
    f->started = now_ms;
    f->back_since = f->plan_at = f->restart_at = f->get_at = f->copy_due = 0u;
    f->restart_soon = false;
    link_img_free(&f->copy_img);
    memset(&f->w, 0, sizeof(f->w));
    memset(&f->s, 0, sizeof(f->s));
    memset(&f->log, 0, sizeof(f->log));
    memset(f->said_state, 0xFF, sizeof(f->said_state));
    memset(f->said_pc, 0xFF, sizeof(f->said_pc));
    memset(f->said_ver, 0, sizeof(f->said_ver));
    memset(f->said_at, 0, sizeof(f->said_at));
    f->said_own = 0xFFu;
    f->said_own_pc = 0xFFu;
    plink_master_fw_self(&v->m, now_ms, updating(f));
    say(&v->io, "fw: this panel runs %s%s (model %s); keypads follow it", f->running,
        f->confirmed ? "" : " on its self-test", f->device);
}

size_t link_mfw_read(void *ctx, const uint8_t id8[PLINK_ID8_LEN], uint32_t offset, uint8_t *out,
                     size_t len)
{
    link_master_t *v = ctx;
    link_mfw_t *f = v->io.mfw;
    const link_img_t *ims[3] = {&f->spare_img, &f->run, &f->copy_img};
    for (size_t k = 0; k < 3u; k++) {
        const link_img_t *im = ims[k];
        if (im->bytes == NULL || memcmp(im->sha, id8, PLINK_ID8_LEN) != 0 ||
            (k == 2u && f->w.copy.state != PLINK_FWCOPY_READY)) {
            continue;
        }
        if (offset >= im->size) {
            return 0u;
        }
        const size_t n = im->size - offset < len ? im->size - offset : len;
        memcpy(out, im->bytes + offset, n);
        return n;
    }
    return 0u;
}

/* Restarting: off now, on again in FW_DOWN_MS - into its spare slot, or back to prev. */
static void mfw_restart(link_master_t *v, uint64_t now, bool back)
{
    link_mfw_t *f = v->io.mfw;
    f->down = true;
    f->boot_at = now + FW_DOWN_MS;
    f->going_back = back;
    if (back) {
        say(&v->io, "fw: this panel restarts back to %s", f->prev);
    } else {
        say(&v->io, "fw: this panel restarts into %s, its READY keypads with it", f->spare);
    }
}

/* On again: the new version on its self-test, or the one before, confirmed. */
static void mfw_started(link_master_t *v, uint64_t now)
{
    link_mfw_t *f = v->io.mfw;
    f->down = false;
    if (f->going_back) {
        PUT(f->running, f->prev);
        f->confirmed = true;
        (void)link_img_make(&f->run, f->device, f->running);
    } else {
        PUT(f->prev, f->running);
        PUT(f->running, f->spare);
        f->confirmed = false;
        link_img_free(&f->run);
        f->run = f->spare_img; /* the spare slot is the running one now */
        memset(&f->spare_img, 0, sizeof(f->spare_img));
    }
    f->spare[0] = '\0';
    f->going_back = false;
    const link_io_t io = v->io;
    (void)link_master_init(v, v->c, &io, v->store, f->name, f->id, now); /* link_mfw_boot */
}

static const char *row_text(const plink_fw_row_t *r, char *out, size_t cap)
{
    switch (r->state) {
    case PLINK_FWR_UP_TO_DATE: snprintf(out, cap, "up to date"); break;
    case PLINK_FWR_DOWNLOADING: snprintf(out, cap, "downloading %u %%", (unsigned)r->percent); break;
    case PLINK_FWR_RECEIVING:
        snprintf(out, cap, "receiving from this panel %u %%", (unsigned)r->percent);
        break;
    case PLINK_FWR_READY: snprintf(out, cap, "ready, restarts with this panel"); break;
    case PLINK_FWR_RESTARTING: snprintf(out, cap, "restarting"); break;
    case PLINK_FWR_NOT_UPDATED:
        snprintf(out, cap, "not updated yet, tries again by itself (why %u %s)", (unsigned)r->why,
                 why_word(r->why));
        break;
    case PLINK_FWR_NEWER: snprintf(out, cap, "newer than this panel: left alone"); break;
    default: snprintf(out, cap, "no updates through the link"); break;
    }
    return out;
}

static const char *stage_word(uint8_t stage)
{
    switch (stage) {
    case ALARM_FW_KEYPAD_INSTALLING: return "installing an update (it restarts)";
    case ALARM_FW_KEYPAD_UPDATED: return "updated (it passed its self-test)";
    case ALARM_FW_KEYPAD_ROLLED_BACK: return "update rolled back (the previous version runs)";
    case ALARM_FW_KEYPAD_FAILED: return "update failed";
    default: return "?";
    }
}

/* The copy the planner wants: freed when none is wanted, fetched anew for other facts, in
 * FW_COPY_MS from its update server - or failed. */
static void mfw_copy_turn(link_master_t *v, uint64_t now)
{
    link_mfw_t *f = v->io.mfw;
    plink_fw_copy_t *c = &f->w.copy;
    const plink_fw_facts_t *want = &f->s.copy;
    if (want->version[0] == '\0') {
        if (c->state != PLINK_FWCOPY_NONE) {
            say(&v->io, "fw: the copy for keypads is freed (none needs it)");
        }
        memset(c, 0, sizeof(*c));
        link_img_free(&f->copy_img);
    } else if (c->state == PLINK_FWCOPY_NONE || memcmp(&c->facts, want, sizeof(c->facts)) != 0) {
        memset(c, 0, sizeof(*c));
        link_img_free(&f->copy_img);
        c->state = PLINK_FWCOPY_FETCHING;
        c->facts = *want;
        f->copy_due = now + FW_COPY_MS;
        say(&v->io, "fw: fetching a copy of %s for model %s (its keypads can't fetch it)",
            want->version, want->device);
    } else if (c->state == PLINK_FWCOPY_FETCHING && now >= f->copy_due) {
        if (f->server == LINK_SRV_ON && link_img_make(&f->copy_img, c->facts.device,
                                                      c->facts.version)) {
            c->state = PLINK_FWCOPY_READY;
            image_of(&c->image, &f->copy_img, PLINK_FWSRC_COPY);
            say(&v->io, "fw: the copy of %s for model %s is in (%u bytes)", c->facts.version,
                c->facts.device, (unsigned)c->image.size);
        } else {
            c->state = PLINK_FWCOPY_FAILED;
            c->why = f->server == LINK_SRV_REFUSE ? (uint8_t)PLINK_FW_WHY_NOT_OFFERED
                                                  : (uint8_t)PLINK_FW_WHY_NO_CONNECT;
            say(&v->io, "fw: no copy of %s for model %s: its update server %s",
                c->facts.version, c->facts.device, srv_word(f->server));
        }
    }
}

/* Each keypad's row and the step's events, said as they change (a percent once a second). */
static void mfw_say_news(link_master_t *v, uint64_t now)
{
    link_mfw_t *f = v->io.mfw;
    plink_fw_log_ev_t ev[PLINK_FW_LOG_MAX];
    const size_t n = plink_fw_log_step(&f->log, &f->w, &f->s, ev, PLINK_FW_LOG_MAX);
    for (size_t i = 0; i < n; i++) {
        if (ev[i].stage == ALARM_FW_KEYPAD_FAILED) {
            say(&v->io, "event: keypad %u %s: why %u %s", (unsigned)ev[i].device,
                stage_word(ev[i].stage), (unsigned)ev[i].why, why_word(ev[i].why));
        } else {
            say(&v->io, "event: keypad %u %s", (unsigned)ev[i].device, stage_word(ev[i].stage));
        }
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &f->w.kp[i];
        const plink_fw_row_t *r = &f->s.row[i];
        if (kp->device == 0u || r->state == PLINK_FWR_NONE) {
            continue;
        }
        const char *ver = kp->have_info ? kp->info.version : "?";
        const bool moved = r->state != f->said_state[i] || strcmp(ver, f->said_ver[i]) != 0;
        const bool progress = (r->state == PLINK_FWR_DOWNLOADING ||
                               r->state == PLINK_FWR_RECEIVING) &&
                              r->percent != f->said_pc[i] && now >= f->said_at[i] + SAY_PROGRESS_MS;
        if (!moved && !progress) {
            continue;
        }
        f->said_state[i] = r->state;
        f->said_pc[i] = r->percent;
        PUT(f->said_ver[i], ver);
        f->said_at[i] = now;
        char text[96];
        say(&v->io, "fw: keypad %u on %s: %s", (unsigned)kp->device, ver,
            row_text(r, text, sizeof(text)));
    }
}

/* One planner step, as plink_fwm_svc.c runs it: the world, the step, its orders, the copy, the
 * news, and its own restart - allowed once its update is in its spare slot, it is confirmed,
 * everything is disarmed and no keypad installs alone (keypads installing with it, once its
 * INSTALLs are out, hold nothing); then when its INSTALLs are echoed, or 5 s on. */
static void mfw_plan(link_master_t *v, uint64_t now)
{
    link_mfw_t *f = v->io.mfw;
    plink_fw_world_t *w = &f->w;
    const bool spare = f->spare[0] != '\0';
    PUT(w->target, spare ? f->spare : f->running);
    PUT(w->running, f->confirmed ? f->running : "");
    PUT(w->server, FW_SERVER);
    PUT(w->channel, FW_CHANNEL);
    memset(w->key, FW_KEY_BYTE, sizeof(w->key));
    if (spare) {
        image_of(&w->own, &f->spare_img, PLINK_FWSRC_SPARE);
    } else if (f->confirmed) {
        image_of(&w->own, &f->run, PLINK_FWSRC_RUNNING); /* worked out once confirmed */
    } else {
        memset(&w->own, 0, sizeof(w->own));
    }
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        (void)plink_master_fw_keypad(&v->m, now, d, &w->kp[d - 1u]);
    }
    const bool busy = plink_master_fw_busy(&v->m, now);
    /* Its keypads download alongside it and restart with it (decision 7): its own restart waits
     * while one on line still gets the target, for at most 30 min - the rows of this step planned
     * without the restart first (an order this step sends counts). */
    w->own_restart = false;
    plink_fw_plan(w, now, &f->s);
    bool getting = false;
    for (size_t i = 0; spare && i < PLINK_KEYPADS_MAX; i++) {
        const uint8_t r = f->s.row[i].state;
        getting = getting || (w->kp[i].online &&
                              (r == PLINK_FWR_DOWNLOADING || r == PLINK_FWR_RECEIVING));
    }
    const bool waits = getting && now < f->spare_at + LINK_FW_KEYPADS_WAIT_MS;
    const bool allowed = spare && f->confirmed && !plink_master_fw_hold(&v->m) && !waits &&
                         (!busy || f->restart_at != 0u);
    w->own_restart = allowed;
    plink_fw_plan(w, now, &f->s);
    memcpy(w->mem, f->s.mem, sizeof(w->mem));
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        const plink_fw_order_t *o = &f->s.send[d - 1u];
        if (o->op != 0u) {
            (void)plink_master_fw_order(&v->m, now, d, o);
        }
    }
    mfw_copy_turn(v, now);
    mfw_say_news(v, now);
    /* its own update's line */
    uint8_t own = 0u;
    if (spare) {
        own = allowed ? 4u : waits ? 3u : busy ? 2u : 1u;
    }
    if (own != f->said_own && own != 0u) {
        f->said_own = own;
        static const char *const OWN[] = {"", "it restarts once everything is disarmed",
                                          "a keypad is checking its update",
                                          "its keypads are getting it"};
        if (own == 4u) {
            say(&v->io, "fw: this panel's restart into %s is allowed: INSTALL to its READY keypads",
                f->spare);
        } else {
            say(&v->io, "fw: this panel's update %s waits: %s", f->spare, OWN[own]);
        }
    } else if (own == 0u) {
        f->said_own = 0u;
    }
    if (!allowed) {
        if (f->restart_at != 0u) {
            say(&v->io, "fw: this panel's restart is put off (no longer allowed)");
        }
        f->restart_at = 0u;
    } else if (f->restart_at == 0u) {
        f->restart_at = now;
    } else if (f->s.installing == 0u || now >= f->restart_at + FW_INSTALL_WAIT) {
        f->restart_at = 0u;
        mfw_restart(v, now, false);
    }
}

/* LINK-041: a keypad with updates back on line (a live session heard within 30 s). */
static bool keypad_back(link_master_t *v, uint64_t now)
{
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        plink_fw_kp_t kp;
        if (plink_master_fw_keypad(&v->m, now, d, &kp) && kp.online) {
            return true;
        }
    }
    return false;
}

static void mfw_self_test(link_master_t *v, uint64_t now)
{
    link_mfw_t *f = v->io.mfw;
    const bool paired = plink_master_held(&v->m) != 0u;
    if (f->back_since == 0u && keypad_back(v, now)) {
        f->back_since = now;
    }
    if (f->bad_test) {
        if (now >= f->started + MFW_BAD_MS) {
            f->bad_test = false;
            say(&v->io, "fw: this panel's self-test failed (made to fail): it goes back to %s",
                f->prev);
            mfw_restart(v, now, true);
        }
        return;
    }
    const bool passed = now >= f->started + MFW_TEST_MS &&
                        (!paired || (f->back_since != 0u && now >= f->back_since + MFW_BACK_MS));
    if (passed) {
        f->confirmed = true;
        f->plan_at = now; /* INFO names it at once */
        say(&v->io, "fw: this panel's self-test passed: %s confirmed", f->running);
        plink_master_fw_self(&v->m, now, updating(f));
    } else if (paired && f->back_since == 0u && now >= f->started + MFW_TEST_MAX_MS) {
        say(&v->io, "fw: this panel's self-test failed (no keypad back): it goes back to %s",
            f->prev);
        mfw_restart(v, now, true);
    }
}

bool link_mfw_tick(link_master_t *v, uint64_t now_ms)
{
    link_mfw_t *f = v->io.mfw;
    if (f->down) {
        if (now_ms >= f->boot_at) {
            mfw_started(v, now_ms);
        }
        return false;
    }
    if (f->getting[0] != '\0' && now_ms >= f->get_at) {
        f->get_at = now_ms + FW_STEP_MS;
        if (f->spare_img.bytes == NULL && !link_img_make(&f->spare_img, f->device, f->getting)) {
            say(&v->io, "fw: no memory for the download");
            f->getting[0] = '\0';
        } else {
            f->got = f->got + FW_BPS_STEP < f->spare_img.size ? f->got + FW_BPS_STEP
                                                               : f->spare_img.size;
            const unsigned pc = (unsigned)((uint64_t)f->got * 100u / f->spare_img.size);
            if (f->got == f->spare_img.size) {
                PUT(f->spare, f->getting);
                f->getting[0] = '\0';
                f->spare_at = now_ms;
                f->plan_at = now_ms;
                say(&v->io, "fw: this panel's update %s is downloaded and checked: its keypads "
                            "get it too",
                    f->spare);
            } else if (pc / 10u != f->said_own_pc / 10u || f->said_own_pc == 0xFFu) {
                f->said_own_pc = (uint8_t)pc;
                say(&v->io, "fw: this panel downloading %s %u %%", f->getting, pc);
            }
        }
    }
    plink_master_fw_self(&v->m, now_ms, updating(f)); /* STATUS's UPDATING (a change sends one) */
    if (!f->confirmed) {
        mfw_self_test(v, now_ms);
        if (f->down) {
            return false;
        }
    }
    if (now_ms >= f->plan_at) {
        f->plan_at = now_ms + (f->restart_at != 0u ? FW_PLAN_FAST_MS : FW_PLAN_MS);
        mfw_plan(v, now_ms);
        if (f->restart_at != 0u) {
            f->plan_at = now_ms + FW_PLAN_FAST_MS;
        }
    }
    return !f->down;
}

uint64_t link_mfw_next(const link_master_t *v)
{
    const link_mfw_t *f = v->io.mfw;
    if (f->down) {
        return f->boot_at;
    }
    uint64_t t = f->plan_at;
    if (f->getting[0] != '\0' && f->get_at < t) {
        t = f->get_at;
    }
    if (f->w.copy.state == PLINK_FWCOPY_FETCHING && f->copy_due < t) {
        t = f->copy_due;
    }
    return t;
}

void link_mfw_install(link_master_t *v, uint64_t now_ms, const char *version)
{
    link_mfw_t *f = v->io.mfw;
    proto_version_t pv;
    if (!proto_version_parse(version, &pv)) {
        say(&v->io, "fw install V: a version like 1.5.0");
        return;
    }
    if (!f->confirmed || f->getting[0] != '\0' || f->spare[0] != '\0') {
        say(&v->io, "fw: an update runs already (or this panel is on its self-test)");
        return;
    }
    if (!proto_version_is_upgrade(f->running, version, false)) {
        say(&v->io, "fw: %s is not newer than %s", version, f->running);
        return;
    }
    PUT(f->getting, version);
    f->got = 0u;
    f->get_at = now_ms;
    f->said_own_pc = 0xFFu;
    link_img_free(&f->spare_img);
    memset(&f->w.copy, 0, sizeof(f->w.copy)); /* a copy is of the version that runs now */
    link_img_free(&f->copy_img);
    plink_master_fw_self(&v->m, now_ms, true);
    say(&v->io, "fw: Install %s: downloading (the alarm keeps working); it restarts once "
                "everything is disarmed, then its keypads update with it",
        version);
}

bool link_mfw_again(link_master_t *v, uint64_t now_ms, uint8_t device)
{
    link_mfw_t *f = v->io.mfw;
    if (device < PLINK_DEVICE_FIRST || device > PLINK_DEVICE_LAST ||
        (plink_master_held(&v->m) & (1u << (device - 1u))) == 0u) {
        say(&v->io, "fw update N: no keypad %u", (unsigned)device);
        return false;
    }
    plink_fw_again(&f->w.mem[device - 1u]);
    f->plan_at = now_ms;
    say(&v->io, "fw: keypad %u asked again", (unsigned)device);
    return true;
}

void link_mfw_say(link_master_t *v, uint64_t now_ms)
{
    link_mfw_t *f = v->io.mfw;
    char own[96] = "";
    if (f->getting[0] != '\0') {
        snprintf(own, sizeof(own), "; downloading %s %u %%", f->getting,
                 f->spare_img.size != 0u ? (unsigned)((uint64_t)f->got * 100u / f->spare_img.size)
                                         : 0u);
    } else if (f->spare[0] != '\0') {
        snprintf(own, sizeof(own), "; %s waits in its spare slot%s", f->spare,
                 plink_master_fw_hold(&v->m) ? " (not everything is disarmed)" : "");
    }
    say(&v->io, "fw: this panel runs %s%s (model %s)%s; its update server %s%s", f->running,
        f->confirmed ? "" : " on its self-test", f->device, own, srv_word(f->server),
        f->bad_test ? "; its next self-test fails" : "");
    say(&v->io, "fw: the copy for keypads: %s",
        f->w.copy.state == PLINK_FWCOPY_READY      ? "ready"
        : f->w.copy.state == PLINK_FWCOPY_FETCHING ? "being fetched"
        : f->w.copy.state == PLINK_FWCOPY_FAILED   ? "failed"
                                                   : "none");
    bool any = false;
    for (uint8_t d = PLINK_DEVICE_FIRST; d <= PLINK_DEVICE_LAST; d++) {
        plink_fw_kp_t kp;
        if (!plink_master_fw_keypad(&v->m, now_ms, d, &kp)) {
            continue;
        }
        any = true;
        const plink_fw_row_t *r = &f->s.row[d - 1u];
        char text[96];
        say(&v->io, "fw: keypad %u on %s, model %s: %s%s%s", (unsigned)d,
            kp.have_info ? kp.info.version : "?", kp.have_info ? kp.info.device : "?",
            row_text(r, text, sizeof(text)), kp.online ? "" : " (off line)",
            kp.busy ? " (its supervision held: restarting)" : "");
    }
    if (!any) {
        say(&v->io, "fw: no keypads");
    }
}

/* ---- the keypad ------------------------------------------------------------------------------ */

void link_kfw_setup(link_kfw_t *f, const char *name, const char *panel_id,
                    const plink_addr_t *master, const char *device, const char *version)
{
    memset(f, 0, sizeof(*f));
    PUT(f->name, name != NULL ? name : "PC keypad");
    PUT(f->panel_id, panel_id != NULL ? panel_id : "1c2d3e");
    if (master != NULL) {
        f->have_find = true;
        f->find = *master;
    }
    PUT(f->device, device != NULL ? device : LINK_FW_DEVICE);
    PUT(f->running, version);
    PUT(f->server, FW_KP_SERVER);
    f->srv = LINK_SRV_ON;
}

static void kfw_drop(link_kfw_t *f)
{
    free(f->buf);
    f->buf = NULL;
    link_img_free(&f->img);
    f->run = 0u;
    f->run_op = 0u;
    f->v[0] = '\0';
    f->size = f->wrote = 0u;
    f->ready = false;
    f->asks = 0u;
}

/* The cap its core gives (LINK-029), as panel_ota_set_version_cap applies it. */
static void kfw_cap(link_keypad_t *v)
{
    link_kfw_t *f = v->io.kfw;
    const char *cv = "";
    const bool capped = plink_keypad_version_cap(&v->k, &cv, NULL);
    if (capped == f->capped && strcmp(cv, f->cap) == 0) {
        return;
    }
    f->capped = capped;
    PUT(f->cap, cv);
    if (!capped) {
        say(&v->io, "fw: no cap (not paired)");
    } else if (cv[0] == '\0') {
        say(&v->io, "fw: capped: nothing installs until its master's version is heard");
    } else {
        say(&v->io, "fw: capped at its master's %s: nothing past it", cv);
    }
}

void link_kfw_boot(link_keypad_t *v, uint64_t now_ms)
{
    link_kfw_t *f = v->io.kfw;
    kfw_drop(f);
    f->down = false;
    f->down_at = f->boot_at = 0u;
    f->online_from = 0u;
    f->capped = false;
    f->cap[0] = '\0';
    memset(&f->said, 0, sizeof(f->said));
    f->said.state = 0xFFu;
    f->said_at = 0u;
    /* its master's version back from link/mver, so the cap holds before the first INFO */
    plink_keypad_master_version_restore(&v->k, f->mver, f->mver_net);
    (void)plink_keypad_master_version(&v->k, &f->mver_seq);
    if (plink_keypad_master_version(&v->k, NULL)[0] == '\0' && f->mver[0] != '\0') {
        f->mver[0] = '\0'; /* another pairing's: erased */
        f->mver_net = 0u;
    }
    f->capped = !plink_keypad_version_cap(&v->k, NULL, NULL); /* (said once below) */
    kfw_cap(v);
    plink_fw_info_t info;
    memset(&info, 0, sizeof(info));
    info.can = PLINK_FWI_FETCH | PLINK_FWI_RELAY | PLINK_FWI_KEY_ACCEPT;
    PUT(info.device, f->device);
    PUT(info.chip_rev, "v1");
    info.chip_rev_full = 100u;
    PUT(info.project, LINK_FW_PROJECT);
    PUT(info.version, f->running);
    info.slot = LINK_FW_SLOT;
    memset(info.own_key8, FW_KEY_BYTE, sizeof(info.own_key8));
    if (!zeros(f->key, sizeof(f->key))) {
        memcpy(info.accepted_key8, f->key, sizeof(info.accepted_key8));
    }
    plink_keypad_fw_info(&v->k, now_ms, &info);
    plink_keypad_fw_last(&v->k, now_ms, f->probation ? 1u : 0u, f->last, f->last_version,
                         f->failed);
    if (f->probation && f->ordered[0] != '\0' && strcmp(f->ordered, f->running) == 0) {
        plink_keypad_fw_trial(&v->k, now_ms, f->running); /* on trial with its master */
    }
    say(&v->io, "fw: this keypad runs %s%s (model %s)", f->running,
        f->probation ? (f->ordered[0] != '\0' ? " on its self-test, on trial with its master"
                                              : " on its self-test")
                     : "",
        f->device);
}

size_t link_kfw_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len,
                      uint32_t *busy_ms)
{
    link_keypad_t *v = ctx;
    link_kfw_t *f = v->io.kfw;
    if (f->run == 0u || f->run_op != PLINK_FW_OP_RELAY || f->buf == NULL || offset != f->wrote ||
        (uint64_t)offset + len > f->size) {
        return 0u;
    }
    memcpy(f->buf + offset, data, len);
    f->wrote += (uint32_t)len;
    *busy_ms = 1u;
    return len;
}

static void kfw_report(link_keypad_t *v, uint64_t now, uint8_t state, uint8_t why)
{
    plink_keypad_fw_report(&v->k, now, v->io.kfw->run, state, why);
}

/* FETCH or a new RELAY: the run the order names (whatever it held ends); the order's server and key
 * its own from now on when they differ (LINK-036: nobody is asked). */
static void kfw_begin(link_keypad_t *v, uint64_t now, const plink_fw_order_t *o)
{
    link_kfw_t *f = v->io.kfw;
    kfw_drop(f);
    const bool srv = o->server[0] != '\0' && strcmp(o->server, f->server) != 0;
    const bool key = !zeros(o->key, sizeof(o->key)) && memcmp(o->key, f->key, sizeof(f->key)) != 0;
    if (srv || key) {
        if (srv) {
            PUT(f->server, o->server);
        }
        if (key) {
            memcpy(f->key, o->key, sizeof(f->key));
        }
        say(&v->io, "fw: takes its master's update server %s and key %02x%02x%02x%02x (LINK-036)",
            f->server, f->key[0], f->key[1], f->key[2], f->key[3]);
    }
    f->run = o->order;
    f->run_op = o->op;
    PUT(f->v, o->version);
    if (o->op == PLINK_FW_OP_FETCH) {
        if (f->srv != LINK_SRV_ON) {
            const uint8_t why = f->srv == LINK_SRV_OFF ? (uint8_t)PLINK_FW_WHY_NO_CONNECT
                                                       : (uint8_t)PLINK_FW_WHY_NOT_OFFERED;
            say(&v->io, "fw: FETCH %s: its update server %s (why %u %s)", o->version,
                srv_word(f->srv), (unsigned)why, why_word(why));
            kfw_report(v, now, PLINK_FWS_FAILED, why);
            kfw_drop(f);
            return;
        }
        if (!link_img_make(&f->img, f->device, o->version)) {
            kfw_report(v, now, PLINK_FWS_FAILED, PLINK_FW_WHY_NO_MEMORY);
            kfw_drop(f);
            return;
        }
        f->size = f->img.size;
        memcpy(f->sha, f->img.sha, sizeof(f->sha));
        f->fetch_at = now + FW_STEP_MS;
        say(&v->io, "fw: FETCH %s from its update server %s", o->version, f->server);
        plink_keypad_fw_progress(&v->k, now, f->run, 0u, f->size, f->sha);
        kfw_report(v, now, PLINK_FWS_FETCHING, PLINK_FW_WHY_OK);
        return;
    }
    f->size = o->size;
    memcpy(f->sha, o->sha256, sizeof(f->sha));
    f->buf = malloc(o->size != 0u ? o->size : 1u);
    if (f->buf == NULL) {
        kfw_report(v, now, PLINK_FWS_FAILED, PLINK_FW_WHY_NO_MEMORY);
        kfw_drop(f);
        return;
    }
    say(&v->io, "fw: RELAY %s from its master, %u bytes", o->version, (unsigned)o->size);
    kfw_report(v, now, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK);
}

/* Restarting in FW_KP_GOES_MS (its FW_STATE goes first), on again FW_DOWN_MS after. */
static void kfw_restart(link_kfw_t *f, uint64_t now)
{
    f->down_at = now + FW_KP_GOES_MS;
    f->boot_at = f->down_at + FW_DOWN_MS;
}

static void kfw_self_test(link_keypad_t *v, uint64_t now)
{
    link_kfw_t *f = v->io.kfw;
    const uint8_t trial = plink_keypad_fw_trial_state(&v->k, now);
    const bool up = f->online_from != 0u && now >= f->online_from + KFW_TEST_MS;
    if (trial == PLINK_FWT_FAIL || (up && f->bad_test)) {
        say(&v->io, "fw: self-test failed (%s): back to %s",
            trial == PLINK_FWT_FAIL ? "its master went back, or never named this version"
                                    : "made to fail",
            f->prev);
        f->last = PLINK_FW_LAST_ROLLED_BACK;
        PUT(f->last_version, f->running);
        f->failed = trial == PLINK_FWT_FAIL ? KFW_FAILED_TRIAL : KFW_FAILED_TEST;
        PUT(f->running, f->prev);
        f->probation = false;
        f->ordered[0] = '\0';
        f->bad_test = false;
        kfw_restart(f, now);
    } else if (up && trial != PLINK_FWT_WAIT) {
        f->probation = false;
        f->ordered[0] = '\0';
        f->last = PLINK_FW_LAST_UPDATED;
        PUT(f->last_version, f->running);
        f->failed = 0u;
        plink_keypad_fw_last(&v->k, now, 0u, f->last, f->last_version, 0u);
        say(&v->io, "fw: self-test passed%s: %s confirmed",
            trial == PLINK_FWT_PASS ? " (its master runs it)" : "", f->running);
    }
}

/* Its run as FW_STATE says it, said as it changes (a percent once a second). */
static void kfw_say_news(link_keypad_t *v, uint64_t now)
{
    link_kfw_t *f = v->io.kfw;
    plink_fw_state_t st;
    plink_keypad_fw_status(&v->k, now, &st);
    const unsigned pc = st.size != 0u ? (unsigned)((uint64_t)st.bytes * 100u / st.size) : 0u;
    const unsigned was = f->said.size != 0u
                             ? (unsigned)((uint64_t)f->said.bytes * 100u / f->said.size)
                             : 0u;
    const bool moved = st.state != f->said.state || st.why != f->said.why ||
                       ((st.flags ^ f->said.flags) & PLINK_FWF_PAUSED) != 0u;
    const bool progress = pc != was && now >= f->said_at + SAY_PROGRESS_MS &&
                          (st.state == PLINK_FWS_FETCHING || st.state == PLINK_FWS_RECEIVING);
    if (!moved && !progress) {
        return;
    }
    f->said = st;
    f->said_at = now;
    const bool paused = (st.flags & PLINK_FWF_PAUSED) != 0u;
    switch (st.state) {
    case PLINK_FWS_FETCHING: say(&v->io, "fw: downloading %s %u %%", st.target, pc); break;
    case PLINK_FWS_RECEIVING:
        say(&v->io, "fw: receiving %s from its master %u %%%s%s%s", st.target, pc,
            paused ? " (paused: " : "", paused ? why_word(st.why) : "", paused ? ")" : "");
        break;
    case PLINK_FWS_VERIFYING: say(&v->io, "fw: checking %s", st.target); break;
    case PLINK_FWS_READY:
        say(&v->io, "fw: %s ready: restarts with its master once everything is disarmed",
            st.target);
        break;
    case PLINK_FWS_INSTALLING: say(&v->io, "fw: installing %s: restarting", st.target); break;
    case PLINK_FWS_FAILED:
        say(&v->io, "fw: the update to %s failed (why %u %s)", st.target, (unsigned)st.why,
            why_word(st.why));
        break;
    case PLINK_FWS_STOPPED: say(&v->io, "fw: the update to %s stopped", st.target); break;
    default: break;
    }
}

bool link_kfw_tick(link_keypad_t *v, uint64_t now_ms)
{
    link_kfw_t *f = v->io.kfw;
    if (f->down) {
        if (now_ms >= f->boot_at) {
            f->down = false;
            const link_io_t io = v->io;
            link_keypad_init(v, v->c, &io, v->store, f->name, f->panel_id,
                             f->have_find ? &f->find : NULL, now_ms); /* link_kfw_boot */
        }
        return false;
    }
    if (f->down_at != 0u) {
        if (now_ms >= f->down_at) {
            f->down_at = 0u;
            f->down = true;
            say(&v->io, "fw: this keypad restarts (off for %u s)", (unsigned)(FW_DOWN_MS / 1000u));
            return false;
        }
        return true; /* its last FW_STATE goes */
    }
    /* its master's version kept (link/mver) and the cap applied whenever it moves */
    uint32_t seq = 0u;
    const char *mv = plink_keypad_master_version(&v->k, &seq);
    if (seq != f->mver_seq) {
        f->mver_seq = seq;
        uint16_t net = 0u;
        (void)plink_keypad_version_cap(&v->k, NULL, &net);
        PUT(f->mver, mv);
        f->mver_net = mv[0] != '\0' ? net : 0u;
        kfw_cap(v);
    }
    plink_keypad_info_t in;
    plink_keypad_info(&v->k, now_ms, &in);
    if (in.state != PLINK_LINK_ONLINE) {
        f->online_from = 0u;
    } else if (f->online_from == 0u) {
        f->online_from = now_ms;
    }
    plink_fw_order_t o;
    while (plink_keypad_take_fw_order(&v->k, &o)) {
        switch (o.op) {
        case PLINK_FW_OP_FETCH:
        case PLINK_FW_OP_RELAY: kfw_begin(v, now_ms, &o); break;
        case PLINK_FW_OP_CANCEL:
            if (f->run != 0u) {
                say(&v->io, "fw: the run for %s ends (%s)", f->v,
                    o.order != 0u ? "its master cancelled it" : "the link core ended it");
            }
            kfw_drop(f);
            break;
        default: break; /* INFO: the same facts; INSTALL: the gate below */
        }
    }
    if (f->run != 0u && !f->ready && f->run_op == PLINK_FW_OP_FETCH && now_ms >= f->fetch_at) {
        f->wrote = f->wrote + FW_BPS_STEP < f->size ? f->wrote + FW_BPS_STEP : f->size;
        f->fetch_at = now_ms + FW_STEP_MS;
        plink_keypad_fw_progress(&v->k, now_ms, f->run, f->wrote, f->size, f->sha);
        if (f->wrote == f->size) {
            f->ready = true; /* its server's SHA-256 checked */
            kfw_report(v, now_ms, PLINK_FWS_READY, PLINK_FW_WHY_OK);
        }
    }
    if (f->run != 0u && !f->ready && f->run_op == PLINK_FW_OP_RELAY && f->buf != NULL &&
        f->wrote == f->size) {
        uint8_t got[PLINK_SHA256_LEN];
        const plink_iov_t part = {f->buf, f->size};
        plink_sha256(&part, 1u, got);
        if (memcmp(got, f->sha, sizeof(got)) == 0) {
            f->ready = true;
            kfw_report(v, now_ms, PLINK_FWS_READY, PLINK_FW_WHY_OK);
        } else {
            kfw_report(v, now_ms, PLINK_FWS_FAILED, PLINK_FW_WHY_BAD_SHA);
            kfw_drop(f);
        }
    }
    /* The gate, asked twice about a second apart, once its master's INSTALL for V is here. */
    if (f->ready && plink_keypad_fw_install_for(&v->k, f->v)) {
        plink_keypad_fw_gate(&v->k, now_ms, 0u); /* no fire or CO tone kept on the PC */
        if (plink_keypad_fw_hold(&v->k, now_ms) != 0u) {
            f->asks = 0u;
        } else if (f->asks == 0u) {
            f->asks = 1u;
            f->ask_at = now_ms + KFW_ASK_MS;
        } else if (now_ms >= f->ask_at) {
            kfw_report(v, now_ms, PLINK_FWS_INSTALLING, PLINK_FW_WHY_OK);
            kfw_say_news(v, now_ms);
            PUT(f->prev, f->running);
            PUT(f->running, f->v);
            PUT(f->ordered, f->v); /* "ordered install of V", kept across the restart */
            f->probation = true;
            f->last = PLINK_FW_LAST_NONE;
            f->last_version[0] = '\0';
            f->failed = 0u;
            kfw_drop(f);
            kfw_restart(f, now_ms);
            return true;
        }
    } else {
        f->asks = 0u;
    }
    if (f->probation) {
        kfw_self_test(v, now_ms);
    }
    kfw_say_news(v, now_ms);
    return true;
}

uint64_t link_kfw_next(const link_keypad_t *v)
{
    const link_kfw_t *f = v->io.kfw;
    if (f->down) {
        return f->boot_at;
    }
    if (f->down_at != 0u) {
        return f->down_at;
    }
    uint64_t t = UINT64_MAX;
    if (f->run != 0u && !f->ready && f->run_op == PLINK_FW_OP_FETCH) {
        t = f->fetch_at;
    }
    if (f->asks != 0u && f->ask_at < t) {
        t = f->ask_at;
    }
    if (f->probation && f->online_from != 0u && f->online_from + KFW_TEST_MS < t) {
        t = f->online_from + KFW_TEST_MS;
    }
    return t;
}

void link_kfw_say(link_keypad_t *v, uint64_t now_ms)
{
    link_kfw_t *f = v->io.kfw;
    static const char *const TRIAL[] = {"none", "waiting for its master", "passed", "failed"};
    const uint8_t trial = plink_keypad_fw_trial_state(&v->k, now_ms);
    say(&v->io, "fw: this keypad runs %s (model %s)%s; trial with its master: %s", f->running,
        f->device, f->probation ? " on its self-test" : "", trial < 4u ? TRIAL[trial] : "?");
    const char *mv = plink_keypad_master_version(&v->k, NULL);
    say(&v->io, "fw: its master runs %s; %s", mv[0] != '\0' ? mv : "(not heard)",
        !f->capped        ? "no cap (not paired)"
        : f->cap[0] == '\0' ? "nothing installs until its master's version is heard"
                            : "nothing installs past it");
    say(&v->io, "fw: its update server %s (%s)%s", f->server, srv_word(f->srv),
        f->bad_test ? "; its next self-test fails" : "");
    plink_fw_state_t st;
    plink_keypad_fw_status(&v->k, now_ms, &st);
    static const char *const RUN[] = {"idle",      "probing", "fetching", "receiving", "key wait",
                                      "verifying", "ready",   "installing", "failed",  "stopped"};
    say(&v->io, "fw: its run: %s %s, %u of %u bytes, why %u %s%s; last %u %s", st.target,
        st.state < 10u ? RUN[st.state] : "?", (unsigned)st.bytes, (unsigned)st.size,
        (unsigned)st.why, why_word(st.why), (st.flags & PLINK_FWF_PAUSED) != 0u ? " (paused)" : "",
        (unsigned)st.last, st.last_version);
}
