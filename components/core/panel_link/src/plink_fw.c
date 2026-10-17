/*
 * plink_fw.c - the master's planner for updates through the link (see
 * include/panel_link/plink_fw.h; docs/features/11 "Updates through the link"; LINK-029, -030,
 * -031, -035).
 *
 * One step: the holds are taken over (those of another version, of a keypad no longer paired, or
 * a stop whose time is up end), each keypad's track is taken over from the last step (or started
 * again for another target), Try again now ends the master's holds, the track is moved by what
 * the master's link core saw (observe), the holds catch keypads stopped or of a held model, then
 * the orders go: CANCEL for runs the planner doesn't follow (and an INSTALL it gave up), INFO,
 * PROBE and FETCH to every keypad at once, the relay's one turn, the install's one turn, INFO
 * naming the master's version once in each keypad's session, and the copy is asked for or freed.
 * The order a keypad is under is kept in its track (op); an order is asked for only on a change of
 * state, so the core's resends are the only repeats.
 */
#include "panel_link/plink_fw.h"

#include <string.h>

#include "alarm/alarm_codec.h" /* alarm_crc32: a keypad's facts */
#include "alarm_proto/ota_profile.h"
#include "alarm_proto/proto_fw.h"

typedef struct {
    const plink_fw_world_t *w;
    plink_fw_step_t *out;
    uint64_t now;
    const char *v;  /* the target: the version the master runs */
    bool runs_v;    /* ... confirmed (its own update may be under way, not its self-test) */
    bool confirmed; /* ... and no update of its own running */
    bool allowed;   /* new orders may go (CANCEL and INFO go anyway) */
    bool settling;  /* the minute after the master started: runs are only taken up */
    bool reach;     /* the master reaches its server (a copy can be fetched) */
} plan_t;

/* ---- small helpers --------------------------------------------------------------------------- */

static bool same(const char *a, const char *b, size_t cap)
{
    return strncmp(a, b, cap) == 0;
}

static void copy_str(char *out, size_t cap, const char *in)
{
    memset(out, 0, cap);
    size_t n = 0u;
    while (in != NULL && n + 1u < cap && in[n] != '\0') {
        n++;
    }
    if (n > 0u) {
        memcpy(out, in, n);
    }
}

static uint64_t nonzero(uint64_t t)
{
    return t != 0u ? t : 1u;
}

/* A keypad's run under way (FW_STATE state): what a CANCEL would end. */
static bool run_active(uint8_t s)
{
    switch (s) {
    case PLINK_FWS_FETCHING:
    case PLINK_FWS_RECEIVING:
    case PLINK_FWS_KEY_WAIT:
    case PLINK_FWS_VERIFYING:
    case PLINK_FWS_READY:
    case PLINK_FWS_INSTALLING: return true;
    default: return false;
    }
}

/* The keypad refused the order it echoes: a refusal (30-49), BUSY (it is restarting), or a class
 * this firmware doesn't know. A run's own why after taking it is OK or waiting (a pause). */
static bool refusal(uint8_t why)
{
    const plink_fw_why_class_t c = plink_fw_why_class(why);
    return c == PLINK_FWC_REFUSED || c == PLINK_FWC_UNKNOWN || why == PLINK_FW_WHY_BUSY;
}

/* The server refuses this keypad (10-12, 16-19, or a 10-19 this firmware doesn't know): never
 * relayed past (LINK-031). Another version, nothing newer and no release of it aren't refusals. */
static bool server_refusal(uint8_t why)
{
    return plink_fw_why_class(why) == PLINK_FWC_SERVER && why != PLINK_FW_WHY_OTHER_VERSION &&
           why != PLINK_FW_WHY_UP_TO_DATE && why != PLINK_FW_WHY_UNKNOWN_VERSION;
}

static void set(plink_fw_track_t *t, uint64_t now, uint8_t state, uint8_t why)
{
    if (t->state != state) {
        t->since_ms = now;
        t->gate_at = 0u; /* the gate's quiet time counts only while it is READY (install_held) */
    }
    t->state = state;
    t->why = why;
}

/* The FW_STATE held is of the keypad's live session and about the order the core holds now: it
 * echoes it, or the core holds none (a run taken up after a master restart). */
static bool fresh(const plink_fw_kp_t *kp)
{
    return kp->have_state && kp->state_now && (kp->order == 0u || kp->state.order == kp->order);
}

/* The order the track is under was echoed as refused. */
static bool refused(const plink_fw_kp_t *kp, const plink_fw_track_t *t)
{
    return t->op != 0u && kp->order != 0u && kp->order_op == t->op && kp->order_acked &&
           kp->order_ms >= t->op_ms && refusal(kp->order_why);
}

/* Heard in a newer session than the one its run was last seen in, with no FW_STATE there yet (a
 * keypad sends one at the first STATUS of a session while a run exists, and every 10 s): it is
 * asked (INFO) until one comes. */
static bool moved(const plink_fw_kp_t *kp, const plink_fw_track_t *t)
{
    return kp->online && !kp->state_now && t->sessions != 0u && kp->sessions != t->sessions;
}

/* Its run is gone: in its live session it says it knows no order and runs nothing (it restarted:
 * no FW_STATE of its run comes again), or it moved and, though asked, no FW_STATE came there for
 * 30 s. */
static bool lost(const plan_t *p, const plink_fw_kp_t *kp, const plink_fw_track_t *t)
{
    if (!kp->online) {
        return false;
    }
    if (kp->state_now && kp->state.order == 0u && kp->order != 0u &&
        !run_active(kp->state.state)) {
        return true;
    }
    return moved(kp, t) && t->moved_ms != 0u && p->now >= t->moved_ms + PLINK_FWP_MOVED_MS;
}

/* Its run's FW_STATE (one every 10 s while a run exists) is a minute late though it is heard and
 * has answered its orders: the run's last one - its end, sent once - was lost, say. */
static bool overdue(const plan_t *p, const plink_fw_kp_t *kp)
{
    return kp->online && kp->state_now && run_active(kp->state.state) &&
           (kp->order == 0u || kp->order_acked) && p->now >= kp->state_ms + PLINK_FWP_LATE_MS;
}

static bool behind(const plan_t *p, const plink_fw_kp_t *kp)
{
    return kp->have_info && proto_version_is_upgrade(kp->info.version, p->v, false);
}

/* Heard now, in a session that carries orders. */
static bool heard(const plink_fw_kp_t *kp)
{
    return kp->device != 0u && kp->fw && kp->online;
}

/* Its CANCEL was refused BUSY: its restart had started (OTA-028) - the INSTALL it holds stays
 * until an order after it is taken, so the CANCEL goes again once its run shows it isn't
 * installing (the restart put off, READY again). */
static bool cancel_busy(const plink_fw_kp_t *kp)
{
    return kp->order != 0u && kp->order_op == PLINK_FW_OP_CANCEL && kp->order_acked &&
           kp->order_why == PLINK_FW_WHY_BUSY;
}

/* Its FW_STATE's last section: it went back from the target. */
static bool rolled_back(const plan_t *p, const plink_fw_state_t *st)
{
    return st->last == PLINK_FW_LAST_ROLLED_BACK &&
           same(st->last_version, p->v, sizeof st->last_version);
}

/* Its FW_STATE's last section: its self-test of the target passed and is over - the only word
 * that makes it UPDATED (a keypad whose service hasn't told its core yet says probation 0 and
 * nothing else, LINK-030). */
static bool passed(const plan_t *p, const plink_fw_state_t *st)
{
    return st->probation == 0u && st->state != PLINK_FWS_INSTALLING &&
           st->last == PLINK_FW_LAST_UPDATED &&
           same(st->last_version, p->v, sizeof st->last_version);
}

static uint32_t crc_str(uint32_t c, const char *s, size_t cap)
{
    size_t n = 0u;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return alarm_crc32(c, s, n);
}

/* A keypad's facts (FW_INFO): a change ends a hold made on them (BLOCKED, a refusal). */
static uint32_t facts_of(const plink_fw_info_t *f)
{
    const uint8_t n[7] = {f->can,
                          (uint8_t)(f->chip_rev_full & 0xFFu),
                          (uint8_t)((unsigned)f->chip_rev_full >> 8),
                          (uint8_t)(f->slot & 0xFFu),
                          (uint8_t)((f->slot >> 8) & 0xFFu),
                          (uint8_t)((f->slot >> 16) & 0xFFu),
                          (uint8_t)((f->slot >> 24) & 0xFFu)};
    uint32_t c = alarm_crc32(0u, n, sizeof n);
    c = crc_str(c, f->device, sizeof f->device);
    c = crc_str(c, f->board_rev, sizeof f->board_rev);
    c = crc_str(c, f->chip_rev, sizeof f->chip_rev);
    c = crc_str(c, f->project, sizeof f->project);
    c = crc_str(c, f->version, sizeof f->version);
    c = crc_str(c, f->cp_version, sizeof f->cp_version);
    c = alarm_crc32(c, f->own_key8, sizeof f->own_key8);
    return alarm_crc32(c, f->accepted_key8, sizeof f->accepted_key8);
}

/* Failed runs within the last 24 h, and the oldest of them. */
static unsigned fails(const plink_fw_track_t *t, uint64_t now, uint64_t *oldest)
{
    unsigned n = 0u;
    uint64_t o = UINT64_MAX;
    for (size_t k = 0; k < PLINK_FWP_BUDGET; k++) {
        if (t->fails[k] != 0u && now < t->fails[k] + PLINK_FWP_BUDGET_MS) {
            n++;
            o = t->fails[k] < o ? t->fails[k] : o;
        }
    }
    if (oldest != NULL) {
        *oldest = o;
    }
    return n;
}

static void fail_add(plink_fw_track_t *t, uint64_t now)
{
    size_t oldest = 0u;
    for (size_t k = 1u; k < PLINK_FWP_BUDGET; k++) {
        if (t->fails[k] < t->fails[oldest]) {
            oldest = k;
        }
    }
    t->fails[oldest] = nonzero(now);
}

/* ---- holds (link/fwgo) ----------------------------------------------------------------------- */

/* The holds are the target's: made while the master runs it (none of another version applies). */
static bool holds_now(const plan_t *p)
{
    return same(p->out->holds.version, p->v, sizeof p->out->holds.version);
}

/* A hold is made only while the master runs the target confirmed - its own update may be under way
 * (it still runs the target), not its self-test (one made then would outlive a version it goes
 * back from). */
static bool holds_make(plan_t *p)
{
    plink_fw_holds_t *h = &p->out->holds;
    if (!p->runs_v) {
        return false;
    }
    if (!holds_now(p)) {
        memset(h, 0, sizeof *h);
        copy_str(h->version, sizeof h->version, p->v);
    }
    return true;
}

static bool stop_held(const plan_t *p, size_t i)
{
    return holds_now(p) && p->out->holds.stop_until_ms[i] != 0u;
}

static int canary_find(const plink_fw_holds_t *h, const plink_fw_info_t *info)
{
    for (size_t k = 0; k < PLINK_FWP_CANARIES; k++) {
        const plink_fw_canary_t *c = &h->canary[k];
        if (c->device[0] != '\0' && same(c->device, info->device, sizeof c->device) &&
            same(c->chip_rev, info->chip_rev, sizeof c->chip_rev)) {
            return (int)k;
        }
    }
    return -1;
}

/* Keypad i's model's entry among the holds of the target (-1: none). */
static int canary_of(const plan_t *p, size_t i)
{
    if (!holds_now(p) || !p->w->kp[i].have_info) {
        return -1;
    }
    return canary_find(&p->out->holds, &p->w->kp[i].info);
}

/* Its device and chip revision are held on their version (LINK-030) - but for the keypad Try again
 * now sends on first. */
static bool canary_held(const plan_t *p, size_t i)
{
    const int k = canary_of(p, i);
    return k >= 0 && p->out->holds.canary[k].retry != i + 1u;
}

/* That keypad is its model's Try again now: a rollback from before in its last section is it. */
static bool canary_retry(const plan_t *p, size_t i)
{
    const int k = canary_of(p, i);
    return k >= 0 && p->out->holds.canary[k].retry == i + 1u;
}

/* What a keypad's going back did to its model's hold (plink_fw_track_t.canary_made). */
enum { CANARY_KEPT = 0, CANARY_ADDED, CANARY_RETRY_ENDED };

/* Keypad i went back: its model is held - again for all of it, when it was its Try again now.
 * Returns what changed (CANARY_*). */
static uint8_t canary_add(plan_t *p, size_t i)
{
    const plink_fw_info_t *info = &p->w->kp[i].info;
    plink_fw_holds_t *h = &p->out->holds;
    if (!holds_make(p)) {
        return CANARY_KEPT;
    }
    const int found = canary_find(h, info);
    if (found >= 0) {
        if (h->canary[found].retry == i + 1u) {
            h->canary[found].retry = 0u;
            return CANARY_RETRY_ENDED;
        }
        return CANARY_KEPT;
    }
    size_t k = 0u;
    while (k + 1u < PLINK_FWP_CANARIES && h->canary[k].device[0] != '\0') {
        k++; /* the first free entry (the last is reused when none is) */
    }
    memset(&h->canary[k], 0, sizeof h->canary[k]);
    copy_str(h->canary[k].device, sizeof h->canary[k].device, info->device);
    copy_str(h->canary[k].chip_rev, sizeof h->canary[k].chip_rev, info->chip_rev);
    return CANARY_ADDED;
}

static void canary_drop(plan_t *p, size_t i)
{
    const int k = canary_of(p, i);
    if (k >= 0) {
        memset(&p->out->holds.canary[k], 0, sizeof p->out->holds.canary[k]);
    }
}

/* Keypad j is of keypad i's model (its device and chip revision), both with their facts. */
static bool same_model(const plan_t *p, size_t i, size_t j)
{
    const plink_fw_kp_t *ki = &p->w->kp[i], *kj = &p->w->kp[j];
    return ki->device != 0u && kj->device != 0u && ki->have_info && kj->have_info &&
           same(ki->info.device, kj->info.device, sizeof ki->info.device) &&
           same(ki->info.chip_rev, kj->info.chip_rev, sizeof ki->info.chip_rev);
}

/* Keypad i's NO_RETURN turned out to be no return at all (it never restarted, late_back): what it
 * did to its model's hold is undone - the hold it made goes, its Try again now is its own again.
 * A hold another keypad of the model went back under stays: a rollback keeps it, and another
 * NO_RETURN takes it over (undone in turn should that one turn out never to have restarted). */
static void canary_undo(plan_t *p, size_t i)
{
    plink_fw_track_t *t = &p->out->track[i];
    const int k = canary_of(p, i);
    if (k >= 0 && t->canary_made == CANARY_ADDED) {
        /* (Another keypad's Try again now under way keeps it too: the hold is that one's now.) */
        bool kept = p->out->holds.canary[k].retry != 0u;
        for (size_t j = 0; j < PLINK_KEYPADS_MAX && !kept; j++) {
            plink_fw_track_t *tj = &p->out->track[j];
            if (j == i || !same_model(p, i, j) || tj->state != PLINK_FWK_FAILED) {
                continue;
            }
            if (tj->why == PLINK_FWK_WHY_ROLLED_BACK) {
                kept = true;
            } else if (tj->why == PLINK_FWK_WHY_NO_RETURN) {
                tj->canary_made = CANARY_ADDED;
                kept = true;
            }
        }
        if (!kept) {
            memset(&p->out->holds.canary[k], 0, sizeof p->out->holds.canary[k]);
        }
    } else if (k >= 0 && t->canary_made == CANARY_RETRY_ENDED &&
               p->out->holds.canary[k].retry == 0u) {
        p->out->holds.canary[k].retry = (uint8_t)(i + 1u);
    }
    t->canary_made = CANARY_KEPT;
}

static bool holds_same(const plink_fw_holds_t *a, const plink_fw_holds_t *b)
{
    if (!same(a->version, b->version, sizeof a->version)) {
        return false;
    }
    for (size_t k = 0; k < PLINK_KEYPADS_MAX; k++) {
        if (a->stop_until_ms[k] != b->stop_until_ms[k]) {
            return false;
        }
    }
    for (size_t k = 0; k < PLINK_FWP_CANARIES; k++) {
        if (!same(a->canary[k].device, b->canary[k].device, sizeof a->canary[k].device) ||
            !same(a->canary[k].chip_rev, b->canary[k].chip_rev, sizeof a->canary[k].chip_rev) ||
            a->canary[k].retry != b->canary[k].retry) {
            return false;
        }
    }
    return true;
}

/* The holds taken over: the master on another version, all of them end ("the master's next
 * version"); a stop ends when its time is up, and with its keypad; a Try again now with its
 * keypad (the model is held whole again). */
static void holds_take(plan_t *p)
{
    plink_fw_holds_t *h = &p->out->holds;
    *h = p->w->holds;
    if (p->runs_v && h->version[0] != '\0' && !holds_now(p)) {
        memset(h, 0, sizeof *h);
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (h->stop_until_ms[i] != 0u &&
            (p->w->kp[i].device == 0u || p->now >= h->stop_until_ms[i])) {
            h->stop_until_ms[i] = 0u;
        }
    }
    for (size_t k = 0; k < PLINK_FWP_CANARIES; k++) {
        const uint8_t r = h->canary[k].retry;
        if (r != 0u && (r > PLINK_KEYPADS_MAX || p->w->kp[r - 1u].device == 0u)) {
            h->canary[k].retry = 0u;
        }
    }
}

void plink_fw_stop(plink_fw_holds_t *h, uint8_t device, const char *version, uint64_t now_ms)
{
    if (h == NULL || version == NULL || version[0] == '\0' || device > PLINK_KEYPADS_MAX) {
        return;
    }
    if (!same(h->version, version, sizeof h->version)) {
        memset(h, 0, sizeof *h);
        copy_str(h->version, sizeof h->version, version);
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (device == 0u || device == i + 1u) {
            h->stop_until_ms[i] = now_ms + PLINK_FWP_HOLD_MS;
        }
    }
}

/* ---- images ---------------------------------------------------------------------------------- */

bool plink_fw_own_fits(const plink_fw_own_t *run, const plink_fw_info_t *info)
{
    if (run == NULL || info == NULL || !run->have || run->image.size == 0u) {
        return false;
    }
    return same(info->device, run->image.device, sizeof info->device) &&
           info->chip_rev_full >= run->chip_min && info->chip_rev_full <= run->chip_max &&
           ota_profile_accepts(info->project, run->image.project) &&
           (info->slot == 0u || run->image.size <= info->slot);
}

/* The copy was fetched with these facts. */
static bool copy_for(const plink_fw_copy_t *c, const plink_fw_info_t *info)
{
    return same(c->device, info->device, sizeof c->device) &&
           same(c->chip_rev, info->chip_rev, sizeof c->chip_rev) &&
           same(c->project, info->project, sizeof c->project);
}

static bool copy_fits(const plan_t *p, const plink_fw_info_t *info)
{
    const plink_fw_copy_t *c = &p->w->copy;
    return c->state == PLINK_FWCOPY_READY && c->image.size > 0u &&
           same(c->image.version, p->v, sizeof c->image.version) && copy_for(c, info) &&
           (info->slot == 0u || c->image.size <= info->slot);
}

static bool own_fits(const plan_t *p, const plink_fw_info_t *info)
{
    return plink_fw_own_fits(&p->w->run, info);
}

/* How keypad i's relay goes now: the image and its path; NULL with *block 0 while it must wait
 * (the running image not worked out yet, or a copy to fetch: *need_copy), or with *block the
 * reason it can't go at all (no RELAY in its FW_INFO, UNKNOWN_VERSION and the own slot doesn't
 * fit, a copy refused for its facts, or no image and no server for a copy: NO_IMAGE). */
static const plink_fw_image_t *relay_way(const plan_t *p, size_t i, uint8_t *path, uint8_t *block,
                                         bool *need_copy)
{
    const plink_fw_info_t *info = &p->w->kp[i].info;
    const plink_fw_copy_t *c = &p->w->copy;
    *path = PLINK_FWK_PATH_NONE;
    *block = 0u;
    if (need_copy != NULL) {
        *need_copy = false;
    }
    if ((info->can & PLINK_FWI_RELAY) == 0u) {
        *block = PLINK_FWK_WHY_NO_IMAGE;
        return NULL;
    }
    if (own_fits(p, info)) {
        *path = PLINK_FWK_PATH_RELAY_OWN;
        return &p->w->run.image;
    }
    if (p->out->track[i].own_only) {
        /* The server has no release of the target for it: its own slot or nothing, never a copy
         * (the same server would answer the copy's check alike). */
        *block = p->w->run.have ? (uint8_t)PLINK_FW_WHY_UNKNOWN_VERSION : 0u;
        return NULL;
    }
    if (copy_fits(p, info)) {
        *path = PLINK_FWK_PATH_RELAY_COPY;
        return &c->image;
    }
    if (c->state == PLINK_FWCOPY_READY && copy_for(c, info) &&
        same(c->image.version, p->v, sizeof c->image.version) && info->slot != 0u &&
        c->image.size > info->slot) {
        /* The copy for its facts is larger than its slot: no image for it here (fetching it
         * again would bring the same). */
        *block = PLINK_FW_WHY_TOO_BIG;
        return NULL;
    }
    if (c->state == PLINK_FWCOPY_FAILED && copy_for(c, info)) {
        /* Never relayed past the server's rules (LINK-031): its refusal of these facts, or of the
         * image itself, blocks; a copy that couldn't be fetched leaves no image. */
        const plink_fw_why_class_t cls = plink_fw_why_class(c->why);
        *block = server_refusal(c->why) || cls == PLINK_FWC_REFUSED ? c->why
                                                                     : PLINK_FWK_WHY_NO_IMAGE;
        return NULL;
    }
    if (!p->w->run.have) {
        return NULL; /* the own slot isn't worked out yet: it may fit */
    }
    if (p->reach) {
        if (need_copy != NULL) {
            *need_copy = true;
        }
        return NULL;
    }
    *block = PLINK_FWK_WHY_NO_IMAGE;
    return NULL;
}

/* The image the master holds whose id8 is the keypad's run's (a relay to go on with). Before its
 * FW_INFO the image its run already took, by id8 and size, is enough: it checks every image
 * itself, and this master ordered that one. */
static const plink_fw_image_t *image_of(const plan_t *p, size_t i, uint8_t *path)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    const uint8_t *id8 = kp->state.id8;
    const plink_fw_copy_t *c = &p->w->copy;
    const bool blind = !kp->have_info;
    if ((blind ? p->w->run.have : own_fits(p, &kp->info)) &&
        memcmp(p->w->run.image.sha256, id8, PLINK_ID8_LEN) == 0 &&
        p->w->run.image.size == kp->state.size) {
        *path = PLINK_FWK_PATH_RELAY_OWN;
        return &p->w->run.image;
    }
    const bool copy = blind ? c->state == PLINK_FWCOPY_READY &&
                                  same(c->image.version, p->v, sizeof c->image.version)
                            : copy_fits(p, &kp->info);
    if (copy && memcmp(c->image.sha256, id8, PLINK_ID8_LEN) == 0 &&
        c->image.size == kp->state.size) {
        *path = PLINK_FWK_PATH_RELAY_COPY;
        return &p->w->copy.image;
    }
    *path = PLINK_FWK_PATH_NONE;
    return NULL;
}

/* ---- orders ---------------------------------------------------------------------------------- */

/* Order op for keypad i (img: a RELAY's image): the core numbers it at once. */
static void emit(plan_t *p, size_t i, uint8_t op, const plink_fw_image_t *img)
{
    plink_fw_order_t *o = &p->out->send[i];
    plink_fw_track_t *t = &p->out->track[i];
    memset(o, 0, sizeof *o);
    o->op = op;
    o->kind = PLINK_FW_KIND_APP;
    if (op != PLINK_FW_OP_CANCEL && (op != PLINK_FW_OP_INFO || p->confirmed)) {
        /* Every order names the version the master runs, and the master orders no other; INFO's
         * is the one a keypad may not pass (docs/features/11 "Never past its master"), so only
         * once that version is confirmed (none on its self-test: it may yet go back). */
        copy_str(o->version, sizeof o->version, p->v);
        if (p->confirmed) {
            t->told = p->w->kp[i].sessions; /* its session hears the master's version */
        }
    }
    if (op == PLINK_FW_OP_PROBE || op == PLINK_FW_OP_FETCH) {
        copy_str(o->server, sizeof o->server, p->w->server);
        copy_str(o->channel, sizeof o->channel, p->w->channel);
    }
    if (op == PLINK_FW_OP_RELAY && img != NULL) {
        o->size = img->size;
        memcpy(o->sha256, img->sha256, sizeof o->sha256);
        memcpy(o->key, img->key, sizeof o->key);
        o->page = PLINK_FWP_PAGE;
        o->count = PLINK_FWP_COUNT;
        o->gap_ms = PLINK_FWP_GAP_MS;
    }
    if (op == PLINK_FW_OP_INSTALL) {
        t->retried = false; /* what comes back of it is new: a new attempt, never seen installing */
        t->tried = false;
    }
    t->voids = 0u; /* (set by the caller for an order that voids an INSTALL) */
    t->op = op;
    t->op_ms = p->now;
}

/* The keypad may hold an INSTALL in its session now: the core's order is that INSTALL (taken, or
 * its echo lost), a CANCEL it refused BUSY, or an order voiding one, not yet echoed. */
static bool may_hold(const plink_fw_kp_t *kp, const plink_fw_track_t *t)
{
    if (kp->order == 0u) {
        return false;
    }
    return kp->order_op == PLINK_FW_OP_INSTALL || cancel_busy(kp) ||
           (t->voids == kp->sessions && kp->order_op == t->op && !kp->order_acked);
}

/* A CANCEL in place of what the core holds: one that voids an INSTALL the keypad may hold is
 * followed until it is echoed (voiding). */
static void cancel(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    const bool voids = may_hold(kp, &p->out->track[i]);
    emit(p, i, PLINK_FW_OP_CANCEL, NULL);
    p->out->track[i].voids = voids ? kp->sessions : 0u;
}

/* Keypad i may still restart on an INSTALL the planner gave up: in the session that INSTALL was
 * of (a new one holds none), the CANCEL or INFO that voids it goes now, is unanswered, or was
 * refused BUSY (its restart under way, or put off still holding it). While it is heard and
 * answers, it holds the install turn (no two keypads restart at once). */
static bool voiding(const plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    const plink_fw_track_t *t = &p->out->track[i];
    if (t->voids == 0u || t->voids != kp->sessions || kp->device == 0u || kp->away) {
        return false; /* (away: unheard, or deaf to orders for 30 s - the turn goes on) */
    }
    if (p->out->send[i].op != 0u) {
        return true;
    }
    return kp->order != 0u && kp->order_op == t->op &&
           (!kp->order_acked || kp->order_why == PLINK_FW_WHY_BUSY);
}

/* It may still hold the INSTALL it took - echoed, then unheard (its restart never seen, or put
 * off) - and the planner gives that INSTALL up: an INFO voids its hold at once, heard or not (any
 * order taken after an INSTALL voids it; the core resends it until it is echoed; in a new session
 * it holds none anyway). The install turn goes on meanwhile. */
static void void_install(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    if (kp->order != 0u && kp->order_op == PLINK_FW_OP_INSTALL && kp->order_acked &&
        p->out->send[i].op == 0u) {
        emit(p, i, PLINK_FW_OP_INFO, NULL);
        p->out->track[i].voids = kp->sessions;
    }
}

/* Its FW_STATE and FW_INFO asked for again (INFO: answered at once, its FW_INFO sent again), at
 * most every 10 s - never over an order still unanswered (the core resends that one; an INFO
 * would replace it - an INSTALL never taken, say), nor in the minute after the master started. */
static void ask(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    const plink_fw_track_t *t = &p->out->track[i];
    if (kp->fw && !p->settling && p->out->send[i].op == 0u &&
        (kp->order == 0u || kp->order_acked) &&
        (t->op != PLINK_FW_OP_INFO || p->now >= t->op_ms + PLINK_FWP_INFO_MS)) {
        emit(p, i, PLINK_FW_OP_INFO, NULL);
    }
}

/* LINK-029: the master's version - the one a keypad may not pass - named in each of its sessions,
 * and again when the confirmed version changes (its track starts again then): INFO, unless an
 * order of that session named it. It waits for every other order (it goes last in a step), never
 * goes over an order still unanswered (a CANCEL lands first), nor to a keypad told to install (its
 * INSTALL names it) or probing (its PROBE's echo is awaited). The track's order stays what it was:
 * the core goes on serving a relay across it. */
static void tell(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    if (!p->confirmed || p->settling || !kp->fw || !kp->online || t->told == kp->sessions ||
        t->installed || t->state == PLINK_FWK_PROBING || p->out->send[i].op != 0u ||
        (kp->order != 0u && !kp->order_acked)) {
        return;
    }
    plink_fw_order_t *o = &p->out->send[i];
    memset(o, 0, sizeof *o);
    o->op = PLINK_FW_OP_INFO;
    o->kind = PLINK_FW_KIND_APP;
    copy_str(o->version, sizeof o->version, p->v);
    t->told = kp->sessions; /* (the core resends it until it is echoed) */
}

/* ---- observing (what the core saw moves the track) ------------------------------------------- */

/* BLOCKED(why), on the facts it has now. */
static void block(plan_t *p, size_t i, uint8_t why)
{
    plink_fw_track_t *t = &p->out->track[i];
    set(t, p->now, PLINK_FWK_BLOCKED, why);
    t->installed = false;
    t->facts = facts_of(&p->w->kp[i].info);
}

/* To the relay (own_only: from the master's own slot only): a keypad without RELAY in its
 * FW_INFO is blocked; the image is chosen at its turn. */
static void to_relay(plan_t *p, size_t i, bool own_only)
{
    plink_fw_track_t *t = &p->out->track[i];
    t->own_only = own_only;
    if ((p->w->kp[i].info.can & PLINK_FWI_RELAY) == 0u) {
        block(p, i, PLINK_FWK_WHY_NO_IMAGE);
        return;
    }
    t->path = PLINK_FWK_PATH_NONE;
    set(t, p->now, PLINK_FWK_RELAY_WAIT, PLINK_FW_WHY_OK);
}

/* Back to BEHIND with nothing counted: probed (or relayed) again. */
static void behind_again(plan_t *p, size_t i)
{
    plink_fw_track_t *t = &p->out->track[i];
    t->installed = false;
    t->tried = false; /* an install of before says nothing of the next */
    t->budget = false;
    t->retry_at = 0u;
    t->away_ms = 0u;
    t->gate_quiet_ms = 0u;
    t->path = PLINK_FWK_PATH_NONE;
    set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK);
}

/* STOPPED: by a person at the keypad (STOPPED) holds it for 24 h, as a stop at the master does. */
static void stopped(plan_t *p, size_t i, uint8_t why)
{
    plink_fw_track_t *t = &p->out->track[i];
    set(t, p->now, PLINK_FWK_STOPPED, why);
    t->installed = false;
    if (why == PLINK_FW_WHY_STOPPED && holds_make(p) && p->out->holds.stop_until_ms[i] == 0u) {
        p->out->holds.stop_until_ms[i] = p->now + PLINK_FWP_HOLD_MS;
    }
}

/* It went back (a rollback, or no return within 10 min): FAILED, and its device and chip revision
 * are held on their version until the master's next one (LINK-030). */
static void went_back(plan_t *p, size_t i, uint8_t why)
{
    plink_fw_track_t *t = &p->out->track[i];
    set(t, p->now, PLINK_FWK_FAILED, why);
    t->installed = false;
    t->retry_at = 0u;
    t->budget = false;
    t->canary_made = canary_add(p, i);
}

/* Its self-test of the target passed: UPDATED, and its model's hold ends - one of it runs the
 * target (a late return; Try again now's keypad). */
static void updated(plan_t *p, size_t i)
{
    plink_fw_track_t *t = &p->out->track[i];
    set(t, p->now, PLINK_FWK_UPDATED, PLINK_FW_WHY_OK);
    t->installed = false;
    t->retried = false;
    canary_drop(p, i);
}

static void failed(plan_t *p, size_t i, uint8_t why, bool counts);

/* The PROBE's answer picks the path (docs/features/11 "Choosing the path, per keypad"). */
static void decide(plan_t *p, size_t i, uint8_t why, const char *offered)
{
    plink_fw_track_t *t = &p->out->track[i];
    const uint8_t can = p->w->kp[i].info.can;
    switch (plink_fw_why_class(why)) {
    case PLINK_FWC_OK:
        /* Exactly the target: FETCH, unless a fetch of it already failed and the relay can go. */
        if (same(offered, p->v, PLINK_FW_VERSION_MAX + 1u) && (can & PLINK_FWI_FETCH) != 0u &&
            !(t->fell_back && (can & PLINK_FWI_RELAY) != 0u)) {
            t->path = PLINK_FWK_PATH_FETCH;
            set(t, p->now, PLINK_FWK_FETCH, PLINK_FW_WHY_OK); /* the order goes in act */
        } else {
            to_relay(p, i, false); /* another version offered */
        }
        return;
    case PLINK_FWC_SERVER:
        if (why == PLINK_FW_WHY_OTHER_VERSION || why == PLINK_FW_WHY_UP_TO_DATE) {
            to_relay(p, i, false);
        } else if (why == PLINK_FW_WHY_UNKNOWN_VERSION) {
            to_relay(p, i, true); /* P2 Q16: a build of the owner's own, from its own slot */
        } else {
            block(p, i, why); /* never relayed past the server's rules */
        }
        return;
    case PLINK_FWC_NOT_REACHED:
    case PLINK_FWC_TRANSFER: to_relay(p, i, false); return;
    case PLINK_FWC_WAITING:
        /* It never asked its server (busy, not quiet): no answer - the relay never goes before the
         * server's word (LINK-031). Probed again after 2 min, nothing counted. */
        failed(p, i, why, false);
        return;
    case PLINK_FWC_STOPPED: stopped(p, i, why); return;
    default: block(p, i, why); return; /* a refusal, or a class unknown */
    }
}

/* A run (or an order) that ended with why; counts: the keypad counts it toward its BUDGET (a run
 * of FETCH or RELAY that failed before READY, as its FW_STATE said or as it went: lost, or
 * unheard 31 min - counted here even where the keypad may not, so its retries stay bounded):
 *   - a person's stop is STOPPED (held 24 h);
 *   - a relay's TIMEOUT paused at its gate is no failed run: tried again after 2 min, not counted;
 *   - a FETCH falls back to the relay once per target for a reach or transfer reason, or when its
 *     check found another version (UNKNOWN_VERSION: from the own slot only);
 *   - the server's refusal blocks;
 *   - BUDGET is tried again once the 3 failed runs this master knows are 24 h old, else hourly;
 *   - a reach or transfer reason is tried again after 2 min, or once 3 failed runs in 24 h are
 *     that old (the keypad's BUDGET); a waiting reason (BUSY) after 2 min, not counted;
 *   - anything else (a refusal) fails for this target, until the keypad's facts change
 *     (LINK-031, LINK-035). */
static void failed(plan_t *p, size_t i, uint8_t why, bool counts)
{
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_why_class_t cls = plink_fw_why_class(why);
    const bool reach = cls == PLINK_FWC_NOT_REACHED || cls == PLINK_FWC_TRANSFER;
    const bool other = why == PLINK_FW_WHY_OTHER_VERSION || why == PLINK_FW_WHY_UP_TO_DATE ||
                       why == PLINK_FW_WHY_UNKNOWN_VERSION;
    t->installed = false;
    t->budget = false;
    if (cls == PLINK_FWC_STOPPED) {
        stopped(p, i, why);
        return;
    }
    if (why == PLINK_FW_WHY_TIMEOUT && t->gate_paused) {
        /* Timed out paused at its gate (armed, an alarm, installer mode): not a failed run - tried
         * again once the master is quiet, so arming never uses up its tries (LINK-035). */
        t->gate_paused = false;
        set(t, p->now, PLINK_FWK_FAILED, why);
        t->retry_at = p->now + PLINK_FWP_RETRY_MS;
        return;
    }
    if (counts) {
        fail_add(t, p->now); /* as the keypad does (a fetch's OTHER_VERSION too) */
    }
    if (t->path == PLINK_FWK_PATH_FETCH && !t->fell_back && (reach || other)) {
        t->fell_back = true;
        to_relay(p, i, why == PLINK_FW_WHY_UNKNOWN_VERSION);
        return;
    }
    if (server_refusal(why)) {
        block(p, i, why);
        return;
    }
    uint64_t oldest = 0u;
    const bool spent = fails(t, p->now, &oldest) >= PLINK_FWP_BUDGET;
    if (why == PLINK_FW_WHY_BUDGET) {
        /* Its own count: tried again once the runs this master knows of are a day old - hourly
         * while it doesn't know all 3 (a master restart forgot them): a refusal costs nothing. */
        set(t, p->now, PLINK_FWK_FAILED, why);
        t->budget = true;
        t->retry_at = spent ? oldest + PLINK_FWP_BUDGET_MS : p->now + PLINK_FWP_BUDGET_ASK_MS;
        return;
    }
    set(t, p->now, PLINK_FWK_FAILED, why);
    if (reach || other) {
        t->budget = spent;
        t->retry_at = spent ? oldest + PLINK_FWP_BUDGET_MS : p->now + PLINK_FWP_RETRY_MS;
    } else if (cls == PLINK_FWC_WAITING) {
        t->retry_at = p->now + PLINK_FWP_RETRY_MS;
    } else {
        t->retry_at = 0u;
        t->facts = facts_of(&p->w->kp[i].info);
    }
}

/* Its PROBE's answer came after its relay went (a check slower than 2 min): a server's refusal
 * still ends the relay - never relayed past the server's rules (LINK-031) - and so does
 * UNKNOWN_VERSION while the own slot doesn't fit (never a copy, P2 decision 16). Blocked: its run
 * is cancelled in act. */
static bool late_refusal(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    const bool relay = t->state == PLINK_FWK_RELAY_WAIT || t->state == PLINK_FWK_RELAY ||
                       t->state == PLINK_FWK_KEY_WAIT ||
                       (t->state == PLINK_FWK_READY && t->path != PLINK_FWK_PATH_FETCH);
    if (!relay || t->probe_ms == 0u || !kp->have_state || st->asked == 0u ||
        kp->state_ms < t->probe_ms) {
        return false;
    }
    const uint64_t age = (uint64_t)st->age_s * 1000u;
    const uint64_t made = kp->state_ms > age ? kp->state_ms - age : 0u;
    if (made + 1000u < t->probe_ms) {
        return false; /* an answer from before its PROBE */
    }
    const uint8_t why = st->probe_why;
    if (!server_refusal(why) &&
        !(why == PLINK_FW_WHY_UNKNOWN_VERSION && kp->have_info && !own_fits(p, &kp->info))) {
        return false;
    }
    block(p, i, why);
    return true;
}

static bool adopt(plan_t *p, size_t i);

static void probing(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (refused(kp, t)) {
        failed(p, i, kp->order_why, false);
        return;
    }
    /* Its echo shows a run for the target under way (its FW_STATE lost after a master restart,
     * say): that run is taken up, the answer not waited for. */
    if (fresh(kp) && kp->order_op == PLINK_FW_OP_PROBE && run_active(st->state) && adopt(p, i)) {
        return;
    }
    if (fresh(kp) && kp->order_op == PLINK_FW_OP_PROBE && st->asked != 0u &&
        st->state != PLINK_FWS_PROBING && kp->state_ms >= t->op_ms) {
        /* An answer made after the PROBE went (its age counted back from when it came). */
        const uint64_t age = (uint64_t)st->age_s * 1000u;
        const uint64_t made = kp->state_ms > age ? kp->state_ms - age : 0u;
        if (made + 1000u >= t->op_ms) {
            decide(p, i, st->probe_why, st->offered);
            return;
        }
    }
    /* No answer in 2 min reads as not reached - but not while its FW_STATEs (every 10 s) say it
     * still probes, up to 10 min: the relay never goes past an answer still to come. */
    const bool still = fresh(kp) && kp->order_op == PLINK_FW_OP_PROBE &&
                       st->state == PLINK_FWS_PROBING &&
                       p->now < kp->state_ms + PLINK_FWP_PROBE_LOST_MS;
    if ((!still && p->now >= t->since_ms + PLINK_FWP_PROBE_WAIT_MS) ||
        p->now >= t->since_ms + PLINK_FWP_PROBE_MAX_MS) {
        to_relay(p, i, false);
    }
}

/* Another keypad holds the relay's turn. */
static bool relay_taken(const plan_t *p, size_t i)
{
    for (size_t j = 0; j < PLINK_KEYPADS_MAX; j++) {
        if (j != i && p->w->kp[j].device != 0u && p->out->track[j].state == PLINK_FWK_RELAY) {
            return true;
        }
    }
    return false;
}

/* FETCH, RELAY, KEY_WAIT: the keypad's run as its FW_STATE says. */
static void running(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (refused(kp, t)) {
        failed(p, i, kp->order_why, false);
        return;
    }
    if (lost(p, kp, t)) {
        failed(p, i, PLINK_FW_WHY_BROKE_OFF, true);
        return;
    }
    if (moved(kp, t) || overdue(p, kp)) {
        ask(p, i); /* its run's end lost, say: nothing is judged before the answer */
        return;
    }
    /* (A relay taken up at KEY_WAIT has no order: this master's core holds no RELAY for it until
     * its key is accepted - its RECEIVING then sends the RELAY again, in relay_turn.) */
    const bool key_wait_relay = t->op == 0u && t->state == PLINK_FWK_KEY_WAIT;
    if (!fresh(kp) || (t->op != PLINK_FW_OP_FETCH && t->op != PLINK_FW_OP_RELAY &&
                       t->op != PLINK_FW_OP_INFO && !key_wait_relay)) {
        return; /* not yet echoed (or an adopted relay waiting to be sent again) */
    }
    switch (st->state) {
    case PLINK_FWS_FETCHING: set(t, p->now, PLINK_FWK_FETCH, PLINK_FW_WHY_OK); return;
    case PLINK_FWS_RECEIVING:
        if (t->state == PLINK_FWK_KEY_WAIT && relay_taken(p, i)) {
            /* Its new key accepted while another keypad has the relay's turn: it waits for its
             * turn again (one RELAY at a time, LINK-030) - its run is cancelled meanwhile, so its
             * RELAY is never served beside the other's; the key stays accepted. */
            to_relay(p, i, t->own_only);
            return;
        }
        set(t, p->now, PLINK_FWK_RELAY, PLINK_FW_WHY_OK);
        t->gate_paused = (st->flags & PLINK_FWF_PAUSED) != 0u &&
                         st->why != PLINK_FW_WHY_NO_STATUS &&
                         plink_fw_why_class(st->why) == PLINK_FWC_WAITING;
        if (t->op == PLINK_FW_OP_INFO) {
            t->op = 0u; /* its relay goes on: the RELAY again (its turn's), as INFO superseded it */
        }
        return;
    case PLINK_FWS_KEY_WAIT: set(t, p->now, PLINK_FWK_KEY_WAIT, PLINK_FW_WHY_OK); return;
    case PLINK_FWS_READY:
        if (same(st->target, p->v, sizeof st->target)) {
            set(t, p->now, PLINK_FWK_READY, PLINK_FW_WHY_OK);
        }
        return;
    case PLINK_FWS_FAILED: failed(p, i, st->why, true); return;
    case PLINK_FWS_STOPPED: stopped(p, i, st->why); return;
    case PLINK_FWS_IDLE:
        failed(p, i, st->why != 0u ? st->why : (uint8_t)PLINK_FW_WHY_BROKE_OFF, true);
        return;
    default: return; /* VERIFYING: still on its way */
    }
}

/* A READY image the keypad discarded (FAILED TIMEOUT: 24 h without its INSTALL), or another end
 * of a run that was READY: no failed run (the keypad counts none), no fall-back used up - a
 * discarded image is fetched or relayed again. */
static void ready_ended(plan_t *p, size_t i, uint8_t why)
{
    if (why == PLINK_FW_WHY_TIMEOUT) {
        behind_again(p, i);
        return;
    }
    failed(p, i, why, false);
}

/* READY, before its INSTALL: waits for its turn. */
static void ready(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (lost(p, kp, t)) {
        failed(p, i, PLINK_FW_WHY_BROKE_OFF, false);
        return;
    }
    if (moved(kp, t) || overdue(p, kp)) {
        ask(p, i);
        return;
    }
    if (!fresh(kp)) {
        return;
    }
    switch (st->state) {
    case PLINK_FWS_INSTALLING: /* restarting into it (an INSTALL of before this master's start) */
        t->installed = true;
        t->tried = true;
        t->sessions = kp->sessions;
        set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK);
        return;
    case PLINK_FWS_FAILED: ready_ended(p, i, st->why); return;
    case PLINK_FWS_STOPPED: stopped(p, i, st->why); return;
    case PLINK_FWS_IDLE: failed(p, i, PLINK_FW_WHY_BROKE_OFF, false); return;
    default: return;
    }
}

/* Keypad i, READY and holding the install turn, is told to install now when it may be (its
 * INSTALL hold is of its session: a new one needs it again). */
static void install_go(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    /* Never before its facts of this session (a relay taken up blind, say: none of the holds has
     * judged it yet). */
    if (p->allowed && !p->settling && kp->fw && kp->online && !kp->away && kp->info_now &&
        behind(p, kp) && p->out->send[i].op == 0u) {
        if (!t->installed) {
            t->gate_quiet_ms = 0u; /* a new turn (not its INSTALL again in a new session) */
        }
        emit(p, i, PLINK_FW_OP_INSTALL, NULL);
        t->sessions = kp->sessions;
        t->installed = true;
    }
}

/* READY and told to install: it holds the install turn until its INSTALL is answered by a restart
 * or an end. Taken (echoed) and then unheard, it is taken for restarting (INSTALLING: NO_RETURN 10
 * min on unless it is heard again, installed). Never given up while it may still be taken:
 * unanswered, its keypad away, it waits for the keypad - the core resends it - for up to 24 h; then
 * a CANCEL replaces it (act) and the turn goes on (a keypad away that long comes back in a new
 * session, which holds no INSTALL). */
static void install_held(plan_t *p, size_t i, bool restarted)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (refused(kp, t)) {
        failed(p, i, kp->order_why, false);
        return;
    }
    const bool took = t->op == PLINK_FW_OP_INSTALL && kp->order_op == PLINK_FW_OP_INSTALL &&
                      kp->order_acked;
    if (!kp->online && (took || restarted)) {
        set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK); /* gone: restarting */
        return;
    }
    if (kp->online) {
        t->away_ms = 0u;
    } else if (t->away_ms == 0u) {
        t->away_ms = nonzero(p->now);
    }
    if ((kp->order != 0u && kp->order_op == PLINK_FW_OP_INSTALL && !kp->order_acked &&
         p->now >= kp->order_ms + PLINK_FWP_HOLD_MS) ||
        (t->away_ms != 0u && p->now >= t->away_ms + PLINK_FWP_HOLD_MS)) {
        behind_again(p, i); /* a day without an answer: its CANCEL goes in act */
        return;
    }
    if (restarted && !kp->info_now) {
        return; /* a new session: its facts first (INFO asked, above) */
    }
    if (!fresh(kp)) {
        return;
    }
    switch (st->state) {
    case PLINK_FWS_INSTALLING:
        t->sessions = kp->sessions;
        t->tried = true;
        set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK);
        return;
    case PLINK_FWS_FAILED: ready_ended(p, i, st->why); return;
    case PLINK_FWS_STOPPED: stopped(p, i, st->why); return;
    case PLINK_FWS_IDLE: failed(p, i, PLINK_FW_WHY_BROKE_OFF, false); return;
    case PLINK_FWS_READY:
        if (t->op != PLINK_FW_OP_INSTALL && same(st->target, p->v, sizeof st->target)) {
            install_go(p, i); /* its INSTALL went with its session, or an INFO replaced it */
            return;
        }
        if (took && kp->online) {
            /* Its gate says no though it took its INSTALL. While the master isn't quiet that is
             * the master's own hold (the keypad's gate says the same); while it is, the keypad's
             * alone (a kept tone, STATUSes that don't reach it, a snapshot that differs), which
             * the master can't see end: after 24 h of the master's quiet the INSTALL is given up
             * - its CANCEL (act) voids the hold and discards the image, and the turn goes on once
             * that is echoed. Fetched or relayed again later, nothing counted. */
            if (t->gate_at != 0u && p->allowed && p->now > t->gate_at) {
                t->gate_quiet_ms += p->now - t->gate_at;
            }
            t->gate_at = nonzero(p->now);
            if (t->gate_quiet_ms >= PLINK_FWP_HOLD_MS) {
                behind_again(p, i);
            }
        }
        return;
    default: return;
    }
}

/* A keypad told to install (installed: READY with its INSTALL, INSTALLING, CHECKING). It went back
 * only on its word that it restarted into the target and left it - never for a frame lost, nor
 * for a keypad unheard or silent (LINK-030: the canary holds its whole model):
 *   - still READY in a new session: it never restarted (a rekey), and holds the turn - its INSTALL
 *     hold went with that session, so INSTALL goes again; installing there: its restart is under
 *     way;
 *   - back on the target in a new session: its self-test (CHECKING), then UPDATED once its last
 *     section says the self-test of the target passed; once it ran the target, its FW_INFO naming
 *     another version in a newer session: it went back - the canary;
 *   - in a new session, its last section saying it rolled back from the target: the canary, so it
 *     is never told to install the target again by itself;
 *   - in a new session, its FW_STATE there saying its run ended (FAILED - its READY image
 *     discarded, TIMEOUT, or another end - or STOPPED): it never restarted, and goes on from there
 *     (a discarded image fetched or relayed again, nothing counted);
 *   - in a new session on another version, its core restarted (no run) though it was never seen
 *     installing, and its last section names no rollback of the target - given, or still not 10
 *     min on (its INSTALLING FW_STATE lost, its service slow): it never tried the target - probed
 *     again, no canary, nothing counted;
 *   - READY: see install_held;
 *   - installing, then READY again in a FW_STATE since while it is heard: its gate said no at the
 *     2nd ask (it holds the turn);
 *   - never back on the target with its self-test passed within 10 min of installing: NO_RETURN
 *     (unheard all along, say, or silent; one that turns out never to have restarted ends that
 *     hold again, late_back). */
static void installed(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    const bool on_target = kp->have_info && same(kp->info.version, p->v, sizeof kp->info.version);
    const bool ready_now = kp->state_now && st->state == PLINK_FWS_READY &&
                           same(st->target, p->v, sizeof st->target);
    const bool installing_now = kp->state_now && st->state == PLINK_FWS_INSTALLING;
    if (kp->sessions != t->sessions && t->state != PLINK_FWK_CHECKING &&
        (ready_now || installing_now)) {
        /* Still READY, or installing, in a new session: it never restarted. Its INSTALL hold went
         * with the session it came in (LINK-034) - INSTALL again (but over one the core still
         * resends); installing, its restart is under way. */
        t->sessions = kp->sessions;
        if (ready_now) {
            t->tried = false;
            set(t, p->now, PLINK_FWK_READY, PLINK_FW_WHY_OK);
            if (kp->order_op != PLINK_FW_OP_INSTALL || kp->order_acked) {
                t->op = 0u;
            }
        } else {
            t->tried = true;
            set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK);
        }
    }
    const bool restarted = kp->sessions != t->sessions;
    if (restarted && kp->online &&
        (!kp->info_now || !kp->state_now || p->now >= kp->state_ms + PLINK_FWP_LATE_MS)) {
        /* In a new session without its FW_INFO or FW_STATE there, or with a FW_STATE a minute old
         * (lost: each goes once - the last section with its self-test's end, or whether it went
         * back), they are asked for again. Nothing is judged on an earlier session's facts
         * meanwhile. */
        ask(p, i);
    }
    if (restarted && kp->info_now && on_target) {
        if (t->state == PLINK_FWK_READY) {
            set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK); /* it restarted into it unseen */
        }
        if (kp->state_now && passed(p, st)) {
            updated(p, i);
            return;
        }
        const uint64_t since = t->since_ms; /* the 10 min count from installing */
        set(t, p->now, PLINK_FWK_CHECKING, PLINK_FW_WHY_OK);
        t->since_ms = since; /* (then 10 min on, still not passed: NO_RETURN) */
    }
    if (restarted && kp->info_now && !on_target && t->state == PLINK_FWK_CHECKING) {
        went_back(p, i, PLINK_FWK_WHY_ROLLED_BACK); /* it ran the target, and runs another now */
        return;
    }
    const bool judged = restarted && kp->state_now && t->state != PLINK_FWK_CHECKING &&
                        !(kp->info_now && on_target);
    if (judged && st->state == PLINK_FWS_FAILED) {
        /* Its FW_STATE of the new session says its run ended there (its last section may name a
         * rollback of an earlier try): it never restarted. */
        ready_ended(p, i, st->why);
        return;
    }
    if (judged && st->state == PLINK_FWS_STOPPED) {
        stopped(p, i, st->why);
        return;
    }
    if (restarted && !on_target && kp->state_now && rolled_back(p, st)) {
        went_back(p, i, PLINK_FWK_WHY_ROLLED_BACK); /* its own word */
        return;
    }
    if (judged) {
        /* Its FW_STATE of the new session, no run of the target (READY and installing above): */
        if (kp->info_now && !t->tried) {
            /* Its core restarted (no run of the target), on another version, never seen
             * installing, its last section naming no rollback of the target (above): once that
             * section is given, or 10 min on, it never tried the target. */
            if (t->state == PLINK_FWK_READY) {
                set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK); /* its 10 min from now */
            }
            if (st->last != PLINK_FW_LAST_NONE || p->now >= t->since_ms + PLINK_FWP_RETURN_MS) {
                behind_again(p, i);
                return;
            }
        }
    }
    if (t->state == PLINK_FWK_READY) {
        install_held(p, i, restarted);
        return;
    }
    if (t->state == PLINK_FWK_INSTALLING && !restarted && fresh(kp) &&
        kp->state_ms >= t->since_ms) {
        /* A FW_STATE since it was taken for installing (one from before - the INSTALL's echo of a
         * keypad gone since - says nothing of it): it never restarted. */
        if (ready_now && kp->online) {
            const uint64_t since = t->since_ms;
            t->tried = false;
            set(t, p->now, PLINK_FWK_READY, PLINK_FW_WHY_OK); /* its gate said no at the 2nd ask */
            t->since_ms = since;
            return;
        }
        if (st->state == PLINK_FWS_FAILED) {
            ready_ended(p, i, st->why);
            return;
        }
        if (st->state == PLINK_FWS_STOPPED) {
            stopped(p, i, st->why);
            return;
        }
    }
    if (p->now >= t->since_ms + PLINK_FWP_RETURN_MS) {
        if (kp->online && on_target && kp->state_now && passed(p, st)) {
            updated(p, i);
        } else {
            went_back(p, i, PLINK_FWK_WHY_NO_RETURN);
            void_install(p, i); /* the turn goes on: it may hold its INSTALL still */
        }
    }
}

/* After a master restart (fresh memory), or with a PROBE's echo: a keypad's run for the target is
 * taken up. A READY one holds no INSTALL of before: its hold went with its session (a master
 * restart is a new one) - it installs at its turn. A relay at KEY_WAIT has no order: this master's
 * core holds no RELAY for it (none since its start, or a PROBE since), so its RELAY goes again at
 * its RECEIVING once its key is accepted (running, relay_turn) - the same image, by id8. */
static bool adopt(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (!kp->have_state || !kp->state_now || !run_active(st->state) ||
        !same(st->target, p->v, sizeof st->target)) {
        return false;
    }
    if (t->voids != 0u && t->op == PLINK_FW_OP_CANCEL && kp->order_op == PLINK_FW_OP_CANCEL &&
        !kp->order_acked) {
        return false; /* an INSTALL given up (its gate's day, say): its run isn't taken up */
    }
    /* A relay by its newest order or its state; once its bytes are in (KEY_WAIT, VERIFYING,
     * READY), by its image being one this master holds (the newest order may be a PROBE since). */
    uint8_t path = PLINK_FWK_PATH_FETCH;
    const plink_fw_image_t *img = image_of(p, i, &path);
    const bool relay = st->op == PLINK_FW_OP_RELAY || st->state == PLINK_FWS_RECEIVING ||
                       (st->state != PLINK_FWS_FETCHING && img != NULL);
    if (!relay) {
        path = PLINK_FWK_PATH_FETCH;
    }
    t->path = path;
    t->sessions = kp->sessions;
    t->installed = false;
    switch (st->state) {
    case PLINK_FWS_INSTALLING:
        t->op = PLINK_FW_OP_INSTALL;
        t->installed = true;
        t->tried = true;
        set(t, p->now, PLINK_FWK_INSTALLING, PLINK_FW_WHY_OK);
        return true;
    case PLINK_FWS_READY:
        t->op = 0u;
        set(t, p->now, PLINK_FWK_READY, PLINK_FW_WHY_OK);
        return true;
    case PLINK_FWS_KEY_WAIT:
        t->op = relay ? 0u : (uint8_t)PLINK_FW_OP_FETCH; /* a fetch goes on by itself */
        set(t, p->now, PLINK_FWK_KEY_WAIT, PLINK_FW_WHY_OK);
        return true;
    default: break;
    }
    if (!relay) {
        t->op = PLINK_FW_OP_FETCH; /* it goes on by itself */
        set(t, p->now, PLINK_FWK_FETCH, PLINK_FW_WHY_OK);
        return true;
    }
    /* A relay: sent again with a new id (it continues by id8) at the relay's turn; one of an
     * image this master doesn't hold starts over there. */
    t->op = 0u;
    set(t, p->now, path != PLINK_FWK_PATH_NONE ? PLINK_FWK_RELAY : PLINK_FWK_RELAY_WAIT,
        PLINK_FW_WHY_OK);
    return true;
}

/* In-progress states: the master's own update - back to BEHIND (CURRENT once on the target); its
 * run is cancelled in act. */
static bool in_flow(uint8_t s)
{
    return s == PLINK_FWK_PROBING || s == PLINK_FWK_FETCH || s == PLINK_FWK_RELAY_WAIT ||
           s == PLINK_FWK_RELAY || s == PLINK_FWK_KEY_WAIT || s == PLINK_FWK_READY;
}

/* NO_RETURN, then heard with a FW_STATE since: one that shows it never restarted into the target
 * didn't go back - still READY for it; its READY image discarded, or its run ended there; its core
 * restarted on another version though it was never seen installing, its last section naming no
 * rollback of the target. Its model's hold is undone (a 10-minute outage of its router while it
 * waited at its gate holds nothing) and it goes on from there. Heard without one, it is asked
 * (INFO: one comes in answer). */
static bool late_back(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (!kp->online) {
        return false;
    }
    if (!kp->state_now || kp->state_ms <= t->since_ms) {
        ask(p, i);
        return false;
    }
    /* (Its run's state says it; the last section may name a rollback of an earlier try.) */
    switch (st->state) {
    case PLINK_FWS_READY:
        if (!same(st->target, p->v, sizeof st->target)) {
            return false;
        }
        canary_undo(p, i);
        t->sessions = kp->sessions;
        set(t, p->now, PLINK_FWK_READY, PLINK_FW_WHY_OK); /* it installs at its turn ... */
        void_install(p, i); /* ... never on its INSTALL of before (INFO, unless one went) */
        return true;
    case PLINK_FWS_FAILED:
        canary_undo(p, i);
        ready_ended(p, i, st->why);
        return true;
    case PLINK_FWS_STOPPED:
        canary_undo(p, i);
        stopped(p, i, st->why);
        return true;
    case PLINK_FWS_IDLE:
        if (t->tried || !kp->info_now || rolled_back(p, st)) {
            return false; /* it restarted into the target, and isn't on it: NO_RETURN stands */
        }
        canary_undo(p, i);
        behind_again(p, i);
        return true;
    default: return false;
    }
}

/* FAILED waits: a retry by time; a refusal until its facts change. A keypad that went back waits
 * for nothing of its own: only Try again now, the master's next version (its track starts again)
 * or its late return on the target end that, so it is never restarted twice for one version -
 * but one NO_RETURN that turns out never to have restarted goes on (late_back). */
static void failed_wait(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    if (t->why == PLINK_FWK_WHY_NO_RETURN && late_back(p, i)) {
        return;
    }
    if (t->why == PLINK_FWK_WHY_ROLLED_BACK || t->why == PLINK_FWK_WHY_NO_RETURN) {
        return;
    }
    if (t->retry_at != 0u) {
        if (p->now >= t->retry_at && p->confirmed) {
            t->retry_at = 0u;
            t->budget = false;
            set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK); /* tried again: PROBE first */
        }
    } else if (facts_of(&kp->info) != t->facts) {
        set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK);
    }
}

/* BLOCKED waits: the canary until its model's hold ends; otherwise probed again 24 h on, when the
 * master's own check was answered since, or when its facts change. */
static void blocked_wait(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    bool again;
    if (t->why == PLINK_FWK_WHY_CANARY) {
        again = !canary_held(p, i);
    } else {
        again = p->now >= t->since_ms + PLINK_FWP_HOLD_MS || p->w->check_ms > t->since_ms ||
                facts_of(&kp->info) != t->facts;
    }
    if (again) {
        set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK);
    }
}

/* Keypad j is sent on again by Try again now: its holds but its BUDGET end, and a rollback from
 * before in its last section no longer holds it. */
static void retry_one(plan_t *p, size_t j)
{
    plink_fw_track_t *t = &p->out->track[j];
    t->retried = true;
    if (t->state == PLINK_FWK_FAILED && t->budget) {
        return;
    }
    if (p->w->kp[j].have_info && !behind(p, &p->w->kp[j])) {
        return; /* on the target already (a NO_RETURN that did return): its word ends its hold */
    }
    if (t->state == PLINK_FWK_FAILED || t->state == PLINK_FWK_BLOCKED ||
        t->state == PLINK_FWK_STOPPED) {
        t->retry_at = 0u;
        t->fell_back = false;
        set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK);
    }
}

/* The keypad Try again now sends on first for a held model (keypad i's), among those of the model
 * still behind the target (one on it already can't try it): the lowest that went back and is
 * heard (P2 decision 13), else the lowest heard - a keypad that went back is often off (its power
 * or network lost mid-install), and one unheard is never ordered, which would keep the others held
 * - else keypad i itself (a number, 1..8). */
static uint8_t canary_pick(const plan_t *p, size_t i)
{
    uint8_t went_back = 0u, any = 0u;
    for (size_t j = 0; j < PLINK_KEYPADS_MAX; j++) {
        const plink_fw_kp_t *kj = &p->w->kp[j];
        const plink_fw_track_t *t = &p->out->track[j];
        if (!same_model(p, i, j) || !behind(p, kj) || !heard(kj)) {
            continue;
        }
        if (went_back == 0u && t->state == PLINK_FWK_FAILED &&
            (t->why == PLINK_FWK_WHY_ROLLED_BACK || t->why == PLINK_FWK_WHY_NO_RETURN)) {
            went_back = (uint8_t)(j + 1u);
        }
        if (any == 0u) {
            any = (uint8_t)(j + 1u);
        }
    }
    return went_back != 0u ? went_back : any != 0u ? any : (uint8_t)(i + 1u);
}

/* The model's Try again now waits on a keypad that can't go on: unheard, or no longer behind the
 * target, and not told to install (an install of it under way ends the hold by its own word). */
static bool retry_stuck(const plan_t *p, uint8_t r)
{
    if (r == 0u || r > PLINK_KEYPADS_MAX) {
        return true;
    }
    const plink_fw_kp_t *kr = &p->w->kp[r - 1u];
    const plink_fw_track_t *tr = &p->out->track[r - 1u];
    if (tr->installed || tr->state == PLINK_FWK_INSTALLING || tr->state == PLINK_FWK_CHECKING) {
        return false;
    }
    return !heard(kr) || (kr->have_info && !behind(p, kr));
}

/* Try again now: the master's holds of keypad i end at once (its stop, a block, a failure
 * waiting); a keypad's BUDGET does not. Its model's canary sends the keypad that went back on
 * first (P2 decision 13), the model's others held until it is UPDATED; pressed again while that
 * keypad can't go on (unheard, say), another of the model is picked. */
static void again(plan_t *p, size_t i)
{
    plink_fw_track_t *t = &p->out->track[i];
    if (p->w->again_ms[i] == 0u || p->w->again_ms[i] <= t->again_ms) {
        return;
    }
    t->again_ms = p->w->again_ms[i];
    p->out->holds.stop_until_ms[i] = 0u;
    const int k = canary_of(p, i);
    if (k >= 0 && retry_stuck(p, p->out->holds.canary[k].retry)) {
        const uint8_t r = canary_pick(p, i);
        p->out->holds.canary[k].retry = r;
        retry_one(p, r - 1u);
    }
    retry_one(p, i);
}

/* Before its FW_INFO (lost, and no INFO goes for a minute after the master starts), what can't
 * wait is taken up by its FW_STATE alone and followed: one installing (it holds the install
 * turn), and a relay of an image this master holds (sent again by id8 before it stalls). The rest
 * waits for its facts. */
static void blind(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    const plink_fw_state_t *st = &kp->state;
    if (t->state == PLINK_FWK_RELAY || t->state == PLINK_FWK_KEY_WAIT) {
        running(p, i);
        return;
    }
    const bool waits = t->state == PLINK_FWK_UNKNOWN || t->state == PLINK_FWK_CURRENT ||
                       t->state == PLINK_FWK_BEHIND || t->state == PLINK_FWK_UPDATED;
    uint8_t path = PLINK_FWK_PATH_NONE;
    if (waits && p->confirmed && kp->state_now &&
        (st->state == PLINK_FWS_INSTALLING ||
         (st->state == PLINK_FWS_RECEIVING && image_of(p, i, &path) != NULL)) &&
        adopt(p, i)) {
        return;
    }
    if (t->state == PLINK_FWK_CURRENT || t->state == PLINK_FWK_BEHIND ||
        t->state == PLINK_FWK_UPDATED) {
        set(t, p->now, PLINK_FWK_UNKNOWN, PLINK_FW_WHY_OK);
    }
}

static void observe(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    if (late_refusal(p, i)) {
        return;
    }
    /* Told to install: followed to its end - but a READY one is cancelled with the others when
     * the master's own update starts (below). */
    const bool told = t->installed && (t->state == PLINK_FWK_INSTALLING ||
                                       t->state == PLINK_FWK_CHECKING ||
                                       (t->state == PLINK_FWK_READY && p->confirmed));
    if (told) {
        installed(p, i);
        return;
    }
    if (!kp->have_info) {
        blind(p, i);
        return;
    }
    const bool old = behind(p, kp);
    if (kp->state_now && t->last_sessions != kp->sessions) {
        /* Its last section, read once in each of its sessions (a restart is a new one): a keypad
         * that went back from the target - while the master restarted, say - holds its model and
         * is never told to install it again by itself; one Try again now sends on again isn't held
         * by its rollback of before. */
        t->last_sessions = kp->sessions;
        if (old && !t->retried && !canary_retry(p, i) && !run_active(kp->state.state) &&
            rolled_back(p, &kp->state)) {
            went_back(p, i, PLINK_FWK_WHY_ROLLED_BACK);
            return;
        }
    }
    if (old && t->state == PLINK_FWK_UPDATED) {
        /* UPDATED on the target, now behind it again: it went back (or a cable put it back) -
         * never told to install the same version again by itself (LINK-030). */
        went_back(p, i, PLINK_FWK_WHY_ROLLED_BACK);
        return;
    }
    if (kp->state_now) {
        t->sessions = kp->sessions; /* its run is seen in this session (lost) */
        t->moved_ms = 0u;
    } else if (kp->sessions != t->sessions && kp->online) {
        if (t->moved_ms == 0u) {
            t->moved_ms = nonzero(p->now); /* counted only while it is heard (it can answer) */
        }
    } else {
        t->moved_ms = 0u;
    }
    if (!p->confirmed && in_flow(t->state)) {
        t->installed = false;
        set(t, p->now, old ? PLINK_FWK_BEHIND : PLINK_FWK_CURRENT, PLINK_FW_WHY_OK);
        return;
    }
    if (!old) {
        /* On the target or newer: left alone (a run of it is cancelled in act). One whose
         * passing the target is news to its model's hold - one that didn't return in time, or
         * the model's Try again now (taken up after a master restart, say) - ends that hold
         * once its self-test has passed: one of the model runs the target. Its word lost (sent
         * once), it is asked for. (A keypad of the model that passed before another went back
         * ends nothing: that rollback is the newer word.) */
        const int k = canary_of(p, i);
        const bool no_return = t->state == PLINK_FWK_FAILED && t->why == PLINK_FWK_WHY_NO_RETURN;
        const bool news = no_return || (k >= 0 && p->out->holds.canary[k].retry == i + 1u);
        if (news && kp->state_now && passed(p, &kp->state)) {
            updated(p, i);
            return;
        }
        if (news && kp->online &&
            (!kp->state_now || (no_return && kp->state_ms <= t->since_ms) ||
             (kp->state.probation != 0u && p->now >= kp->state_ms + PLINK_FWP_LATE_MS))) {
            /* No FW_STATE of this session, none since its NO_RETURN, or one a minute old still on
             * its self-test (the end of it goes once, when it comes): its word is asked for. */
            ask(p, i);
        }
        if (!no_return && t->state != PLINK_FWK_UPDATED) {
            set(t, p->now, PLINK_FWK_CURRENT, PLINK_FW_WHY_OK);
        }
        return;
    }
    switch (t->state) {
    case PLINK_FWK_UNKNOWN:
    case PLINK_FWK_CURRENT:
    case PLINK_FWK_BEHIND: set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK); break; /* found */
    case PLINK_FWK_PROBING: probing(p, i); break;
    case PLINK_FWK_FETCH:
    case PLINK_FWK_RELAY:
    case PLINK_FWK_KEY_WAIT: running(p, i); break;
    case PLINK_FWK_READY: ready(p, i); break;
    case PLINK_FWK_FAILED: failed_wait(p, i); break;
    case PLINK_FWK_BLOCKED: blocked_wait(p, i); break;
    case PLINK_FWK_STOPPED:
        if (!stop_held(p, i)) {
            set(t, p->now, PLINK_FWK_BEHIND, PLINK_FW_WHY_OK); /* its 24 h are over */
        }
        break;
    default: break; /* RELAY_WAIT: its turn decides */
    }
    /* Behind (found so, or a hold just ended): a run of the target it still has is taken up -
     * a READY image is installed, not fetched again. */
    if (t->state == PLINK_FWK_BEHIND && p->confirmed) {
        (void)adopt(p, i);
    }
}

/* The holds catch a keypad that would be updated - behind, or with a run not yet installing (a
 * READY one told to install too); their runs are cancelled in act: a stop (OTA-027) - of a device,
 * so whether or not its FW_INFO is in (a relay taken up blind) - and the canary - the keypads of a
 * model held on its version (LINK-030), known by their FW_INFO (none installs before it). */
static void holds_apply(plan_t *p)
{
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &p->w->kp[i];
        plink_fw_track_t *t = &p->out->track[i];
        if (kp->device == 0u || (t->state != PLINK_FWK_BEHIND && !in_flow(t->state))) {
            continue;
        }
        if (stop_held(p, i)) {
            t->installed = false;
            set(t, p->now, PLINK_FWK_STOPPED, PLINK_FW_WHY_CANCELLED);
        } else if (canary_held(p, i)) {
            block(p, i, PLINK_FWK_WHY_CANARY);
        }
    }
}

/* ---- acting ---------------------------------------------------------------------------------- */

/* The planner follows the keypad's run in these states (a CANCEL there would end its own order). */
static bool follows(uint8_t s)
{
    return s == PLINK_FWK_PROBING || s == PLINK_FWK_FETCH || s == PLINK_FWK_RELAY ||
           s == PLINK_FWK_KEY_WAIT || s == PLINK_FWK_READY || s == PLINK_FWK_INSTALLING ||
           s == PLINK_FWK_CHECKING;
}

/* One keypad's orders outside the two turns: CANCEL for an INSTALL given up and a run the planner
 * doesn't follow, INFO when no FW_INFO came, PROBE (or the relay at once for a keypad that can't
 * fetch) and FETCH. */
static void act(plan_t *p, size_t i)
{
    const plink_fw_kp_t *kp = &p->w->kp[i];
    plink_fw_track_t *t = &p->out->track[i];
    if (kp->device == 0u) {
        return;
    }
    if (kp->order != 0u && kp->order_op == PLINK_FW_OP_INSTALL && !kp->order_acked &&
        !t->installed && t->op != PLINK_FW_OP_CANCEL) {
        /* An INSTALL the planner no longer follows, still unanswered in the core (its keypad away,
         * say): a CANCEL replaces it at once - heard or not - so it is never taken later. */
        cancel(p, i);
        return;
    }
    if (!kp->fw || !kp->online) {
        return;
    }
    if (p->settling || t->installed) {
        /* Runs are only taken up for a minute; a keypad told to install gets its INSTALL only
         * (its turn's, in installed). */
        return;
    }
    /* A run the planner doesn't follow is cancelled - before its facts only when a stop holds it
     * (a relay taken up blind); again when its CANCEL was refused BUSY and its restart was then
     * put off (READY again, still holding its INSTALL: it would restart later on it). */
    if (kp->have_state && kp->state_now && run_active(kp->state.state) &&
        kp->state.state != PLINK_FWS_INSTALLING && !follows(t->state) &&
        (kp->have_info || t->state == PLINK_FWK_STOPPED) &&
        (t->op != PLINK_FW_OP_CANCEL || cancel_busy(kp))) {
        cancel(p, i);
        return;
    }
    if (!kp->have_info) {
        /* Nothing is judged before its facts (but the runs blind takes up). A relay gets no INFO
         * until it answered: its RELAY is sent again first. */
        const bool relay = t->state == PLINK_FWK_RELAY ||
                           (t->state == PLINK_FWK_KEY_WAIT && t->path != PLINK_FWK_PATH_FETCH);
        if (!relay) {
            ask(p, i);
        }
        return;
    }
    if (!p->allowed ||
        (t->op == PLINK_FW_OP_CANCEL && kp->order_op == PLINK_FW_OP_CANCEL && !kp->order_acked)) {
        return; /* a CANCEL lands before anything after it */
    }
    if (t->state == PLINK_FWK_BEHIND) {
        t->path = PLINK_FWK_PATH_NONE;
        t->installed = false;
        if (!kp->info_now) {
            ask(p, i); /* its facts are an earlier session's (this one's FW_INFO lost) */
        } else if ((kp->info.can & PLINK_FWI_FETCH) != 0u) {
            emit(p, i, PLINK_FW_OP_PROBE, NULL); /* PROBE comes first (LINK-031) */
            set(t, p->now, PLINK_FWK_PROBING, PLINK_FW_WHY_OK);
            t->probe_ms = nonzero(p->now);
        } else {
            t->probe_ms = 0u; /* no PROBE this time: no answer of before counts */
            to_relay(p, i, false);
        }
    } else if (t->state == PLINK_FWK_PROBING && t->op == PLINK_FW_OP_PROBE &&
               kp->order_op == PLINK_FW_OP_PROBE && kp->order_acked &&
               p->now >= kp->state_ms + PLINK_FWP_PROBE_LOST_MS &&
               p->now >= t->op_ms + PLINK_FWP_PROBE_LOST_MS) {
        /* Echoed, then silent while a probe sends FW_STATE every 10 s: it is over and its answer
         * (sent once) was lost. Asked again; the 2 min still count from the first. */
        emit(p, i, PLINK_FW_OP_PROBE, NULL);
    } else if (t->state == PLINK_FWK_FETCH && t->op != PLINK_FW_OP_FETCH &&
               t->op != PLINK_FW_OP_INFO) {
        emit(p, i, PLINK_FW_OP_FETCH, NULL); /* (after INFO it goes on by itself) */
    } else if (t->state == PLINK_FWK_READY && !kp->info_now) {
        ask(p, i); /* its facts are an earlier session's: its turn waits for this one's */
    }
}

/* One RELAY receives at a time (LINK-030): a relay taken up is sent again first; then the waiting
 * keypad with the lowest number whose image is here. A relay whose keypad is unheard for 31 min
 * gives the turn back (its own run has timed out, TIMEOUT). */
static void relay_turn(plan_t *p)
{
    bool busy = false;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &p->w->kp[i];
        plink_fw_track_t *t = &p->out->track[i];
        if (t->state != PLINK_FWK_RELAY) {
            continue;
        }
        if (kp->online) {
            t->away_ms = 0u;
        } else if (t->away_ms == 0u) {
            t->away_ms = nonzero(p->now);
        } else if (p->now >= t->away_ms + PLINK_FWP_RELAY_AWAY_MS) {
            t->away_ms = 0u;
            t->gate_paused = false; /* unheard: its link, not its gate */
            failed(p, i, PLINK_FW_WHY_TIMEOUT, true);
            continue;
        }
        if (busy && kp->state_now && kp->state.state == PLINK_FWS_RECEIVING) {
            /* Two relays taken up at once (a master restart while one waited with its key just
             * accepted, say): one receives at a time (LINK-030) - this one waits for its turn
             * again, its run cancelled meanwhile (act), so its RELAY is never served beside the
             * other's. */
            to_relay(p, i, t->own_only);
            continue;
        }
        /* (Not while an INFO waits for its answer: a run that ended, a person's stop say, would
         * start again on a RELAY.) */
        if (t->op != PLINK_FW_OP_RELAY && t->op != PLINK_FW_OP_INFO && p->allowed && kp->fw &&
            kp->online && p->out->send[i].op == 0u) {
            uint8_t path = PLINK_FWK_PATH_NONE;
            const plink_fw_image_t *img = image_of(p, i, &path);
            if (img == NULL) {
                to_relay(p, i, t->own_only); /* that image is gone here: it starts over */
                continue;
            }
            t->path = path;
            emit(p, i, PLINK_FW_OP_RELAY, img); /* the same image: it continues by id8 */
        }
        busy = true;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &p->w->kp[i];
        plink_fw_track_t *t = &p->out->track[i];
        if (t->state != PLINK_FWK_RELAY_WAIT || kp->device == 0u || !kp->have_info) {
            continue;
        }
        uint8_t path = PLINK_FWK_PATH_NONE, why = 0u;
        const plink_fw_image_t *img = relay_way(p, i, &path, &why, NULL);
        if (why != 0u) {
            block(p, i, why);
            continue;
        }
        if (busy || img == NULL || !p->allowed || p->settling || !kp->fw || !kp->online ||
            p->out->send[i].op != 0u) {
            continue;
        }
        t->path = path;
        emit(p, i, PLINK_FW_OP_RELAY, img);
        set(t, p->now, PLINK_FWK_RELAY, PLINK_FW_WHY_OK);
        busy = true;
    }
}

/* One keypad installs at a time (LINK-030): the READY keypad with the lowest number, once none
 * holds the turn - told to install, installing, on its self-test, said busy by the core (its
 * FW_STATE said installing or probation: one this planner doesn't follow too), or heard and
 * answering while the order that voids an INSTALL it took is still on its way (voiding). The
 * keypad goes with its facts of this session (judged by the holds). */
static void install_turn(plan_t *p)
{
    if (!p->allowed || p->settling) {
        return;
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_track_t *t = &p->out->track[i];
        if (t->state == PLINK_FWK_INSTALLING || t->state == PLINK_FWK_CHECKING ||
            (t->state == PLINK_FWK_READY && t->installed) ||
            (p->w->kp[i].device != 0u && p->w->kp[i].busy) || voiding(p, i)) {
            return;
        }
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &p->w->kp[i];
        const plink_fw_track_t *t = &p->out->track[i];
        if (t->state == PLINK_FWK_READY && kp->fw && kp->online && !kp->away && kp->info_now &&
            behind(p, kp) && p->out->send[i].op == 0u) {
            install_go(p, i);
            return;
        }
    }
}

/* One copy at a time: asked for with the facts of the lowest waiting keypad that needs one and
 * can take a relay now (heard); freed when no keypad of its facts needs it - heard and waiting,
 * or relaying from it -, after 30 min unused, or when the master's own update starts. A keypad
 * unheard neither takes the one copy nor keeps it: its copy is fetched once it is heard again. */
static void copy_plan(plan_t *p)
{
    const plink_fw_copy_t *c = &p->w->copy;
    bool needed = false;
    uint8_t want = 0u;
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_kp_t *kp = &p->w->kp[i];
        const plink_fw_track_t *t = &p->out->track[i];
        if (kp->device == 0u || !kp->have_info) {
            continue;
        }
        const bool waits = t->state == PLINK_FWK_RELAY_WAIT && !t->own_only && heard(kp);
        const bool uses = (t->state == PLINK_FWK_RELAY || t->state == PLINK_FWK_KEY_WAIT) &&
                          t->path == PLINK_FWK_PATH_RELAY_COPY;
        if (c->state != PLINK_FWCOPY_NONE && c->state != PLINK_FWCOPY_FAILED &&
            (waits || uses) && copy_for(c, &kp->info) && !own_fits(p, &kp->info) &&
            (c->state == PLINK_FWCOPY_FETCHING ||
             same(c->image.version, p->v, sizeof c->image.version))) {
            needed = true;
        }
        uint8_t path = 0u, why = 0u;
        bool need = false;
        if (waits && want == 0u) {
            (void)relay_way(p, i, &path, &why, &need);
            want = need ? kp->device : 0u;
        }
    }
    if (c->state != PLINK_FWCOPY_NONE) {
        const bool idle =
            c->state == PLINK_FWCOPY_READY && p->now >= c->used_ms + PLINK_FWP_COPY_IDLE_MS;
        if (!needed || idle || !p->confirmed) {
            p->out->copy_free = true;
        }
        return;
    }
    if (want != 0u && p->allowed && !p->settling) {
        p->out->copy_for = want;
    }
}

/* What the master's Software update shows: who is being updated, who waits on a hold that Try
 * again now ends (a keypad's BUDGET it doesn't end). */
static void summary(plan_t *p)
{
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        const plink_fw_track_t *t = &p->out->track[i];
        const uint8_t bit = (uint8_t)(1u << i);
        if (follows(t->state) || t->state == PLINK_FWK_RELAY_WAIT) {
            p->out->running = (uint8_t)(p->out->running | bit);
        }
        if (t->state == PLINK_FWK_STOPPED || t->state == PLINK_FWK_BLOCKED ||
            (t->state == PLINK_FWK_FAILED && !t->budget)) {
            p->out->held = (uint8_t)(p->out->held | bit);
        }
    }
}

/* ---- the step -------------------------------------------------------------------------------- */

void plink_fw_plan(const plink_fw_world_t *w, uint64_t now_ms, plink_fw_step_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (w == NULL) {
        return;
    }
    plan_t p;
    p.w = w;
    p.out = out;
    p.now = now_ms;
    p.v = w->run.image.version;
    p.runs_v = w->own != PLINK_FWM_SELF_TEST && p.v[0] != '\0';
    p.confirmed = w->own == PLINK_FWM_IDLE && p.v[0] != '\0';
    p.allowed = p.confirmed && w->quiet;
    p.settling = now_ms < w->start_ms + PLINK_FWP_SETTLE_MS;
    p.reach = w->check_ms != 0u && now_ms < w->check_ms + PLINK_FWP_CHECK_MS;
    holds_take(&p);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        plink_fw_track_t *t = &out->track[i];
        *t = w->track[i];
        if (w->kp[i].device == 0u) {
            memset(t, 0, sizeof *t); /* no keypad (removed): nothing kept */
        } else if (!same(t->target, p.v, sizeof t->target)) {
            /* Another target starts each keypad again (its master's version named anew); its
             * failed runs in 24 h stay (its BUDGET), and the Try again now it has taken. */
            uint64_t kept[PLINK_FWP_BUDGET];
            const uint64_t taken = t->again_ms;
            memcpy(kept, t->fails, sizeof kept);
            memset(t, 0, sizeof *t);
            memcpy(t->fails, kept, sizeof kept);
            t->again_ms = taken;
            copy_str(t->target, sizeof t->target, p.v);
        }
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (w->kp[i].device != 0u) {
            again(&p, i);
        }
    }
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (w->kp[i].device != 0u) {
            observe(&p, i);
        }
    }
    holds_apply(&p);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        act(&p, i);
    }
    relay_turn(&p);
    install_turn(&p);
    for (size_t i = 0; i < PLINK_KEYPADS_MAX; i++) {
        if (w->kp[i].device != 0u) {
            tell(&p, i);
        }
    }
    copy_plan(&p);
    summary(&p);
    out->holds_changed = !holds_same(&out->holds, &w->holds);
}
