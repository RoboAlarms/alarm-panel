/*
 * host_link_demo.c - the demo panel-link backend (host_link_demo.h): ui_backend_t's link_* over a
 * small model of Harbor House and its keypads on Wi-Fi, for the PC preview's walk and the
 * simulator. Fictional data (RFC 5737 addresses).
 */
#include "host_link_demo.h"

#include <stdio.h>
#include <string.h>

#include "home_control/home_control.h"
#include "ui_backend/ui_dev_model.h"

#define WINDOW_S  120u /* the alarm panel's pairing window (LINK-016) */
#define KEYPADS   8u   /* numbers 1-8 (LINK-001) */
#define PAIR_CODE "588460"
/* A keypad's Smart Home (BL-216): plink_keypad.h's numbers. */
#define DEV_ACTS     2u    /* PLINK_DEV_ACTS_MAX: its device actions in flight */
#define DEV_GIVE_UP  3000  /* PLINK_DEV_ACT_GIVE_UP_MS: an unanswered action is given up */
#define DEV_STALE_MS 30000 /* PLINK_DEV_STALE_MS: no device data this long reads offline */

/* A keypad's own action on a device, once the screen stopped waiting (plink_keypad.h's overlay). */
typedef struct {
    bool used;
    bool given_up;  /* no answer in 3 s: failed and not confirmed */
    uint8_t action; /* dev_action_t */
    int64_t at;     /* sent then (the pretend panel's clock) */
    ui_dev_t was;   /* its device as the alarm panel had it: a later record clears the overlay */
} dev_ovl_t;

/* The alarm panel's own dev_list and dev_act (the pretend house): host_link_demo_backend wraps
 * them, and host_link_demo_init leaves them be. */
static struct {
    size_t (*list)(void *ctx, ui_dev_t *out, size_t max);
    ui_dev_result_t (*act)(void *ctx, const char *ref, uint8_t action, int32_t value,
                           const char *code, alarm_result_t *code_result);
} P;
static ui_dev_t s_rows[UI_DEVL_MAX]; /* kp_dev_act's look at the screen's copy */

static struct {
    bool on;
    ui_link_info_t info;
    ui_link_pair_t pair;
    bool pairing;        /* link_pair_status answers */
    bool looking;        /* ... a keypad's side: it looks (counts up), not a window (counts down) */
    bool master_allowed; /* the alarm panel's installer tapped Yes, allow */
    bool keypad_yes;     /* the keypad's person tapped Yes, pair */
    ui_link_keypad_t kps[KEYPADS];
    size_t n;
    uint16_t held; /* numbers the installer's table and Zigbee keypads hold */
    uint32_t ms;   /* toward the next second */
    ui_link_req_t last;
    bool (*allowed)(void);
    int64_t (*epoch_ms)(void);
    /* Smart Home on a keypad (BL-216) */
    bool dev_stopped;    /* the device data stopped, the alarm panel still answering */
    bool dev_unanswered; /* the alarm panel's answers to device actions are lost */
    int64_t dev_heard;   /* the last device data (0 none) */
    int64_t selection_until;
    dev_ovl_t ovl[DEV_ACTS];
} D;

static int64_t now_ms(void)
{
    return D.epoch_ms != NULL ? D.epoch_ms() : 0;
}

void host_link_demo_init(bool (*allowed)(void), int64_t (*epoch_ms)(void))
{
    memset(&D, 0, sizeof(D));
    D.allowed = allowed;
    D.epoch_ms = epoch_ms;
    D.held = 0x0001u; /* the installer's table: Garage keypad, number 1 */
}

static void master_info(void)
{
    memset(&D.info, 0, sizeof(D.info));
    D.info.role = UI_LINK_ROLE_MASTER;
    uint8_t down = 0;
    for (size_t i = 0; i < D.n; i++) {
        down += D.kps[i].down ? 1u : 0u;
    }
    D.info.keypads = (uint8_t)D.n;
    D.info.keypads_down = down;
}

static void keypad_info(uint8_t state)
{
    memset(&D.info, 0, sizeof(D.info));
    D.info.role = UI_LINK_ROLE_KEYPAD;
    D.info.state = state;
    D.info.device = 2;
    D.info.home = 1;
    D.info.home_gen = 1;
    snprintf(D.info.master_name, sizeof(D.info.master_name), "Harbor House");
    snprintf(D.info.master_addr, sizeof(D.info.master_addr), "192.0.2.42");
}

static void add_keypad(uint8_t device, const char *name, bool connected, bool down, int64_t heard)
{
    if (D.n >= KEYPADS) {
        return;
    }
    ui_link_keypad_t *k = &D.kps[D.n++];
    memset(k, 0, sizeof(*k));
    k->device = device;
    snprintf(k->name, sizeof(k->name), "%s", name);
    k->home = 1;
    k->supervision_s = 300; /* 5 min, the default (decided 2026-10-15) */
    k->connected = connected;
    k->down = down;
    k->waiting = !connected && !down && heard == 0;
    k->heard_epoch_ms = heard;
    snprintf(k->addr, sizeof(k->addr), "192.0.2.%u", 55u + device);
    snprintf(k->fw, sizeof(k->fw), "0.6.0");
}

void host_link_demo_state(host_link_demo_t what)
{
    D.on = what != HOST_LINK_OFF;
    D.pairing = false;
    memset(&D.pair, 0, sizeof(D.pair));
    D.master_allowed = false;
    D.keypad_yes = false;
    D.n = 0;
    D.held = 0x0001u;
    switch (what) {
    case HOST_LINK_MASTER_TWO:
        add_keypad(2, "Hall keypad", true, false, now_ms() - 5000);
        add_keypad(3, "Bedroom keypad", false, true, now_ms() - 35LL * 60000LL);
        master_info();
        break;
    case HOST_LINK_MASTER_THREE:
        add_keypad(2, "Hall keypad", true, false, now_ms() - 5000);
        add_keypad(3, "Bedroom keypad", true, false, now_ms() - 5000);
        add_keypad(4, "Kitchen", true, false, now_ms() - 5000);
        for (size_t i = 0; i < D.n; i++) {
            snprintf(D.kps[i].fw, sizeof(D.kps[i].fw), "0.6.1");
            D.kps[i].fw_state = UI_LINK_FW_UP_TO_DATE;
        }
        D.held = 0x0001u;
        master_info();
        break;
    case HOST_LINK_KEYPAD_CONNECTING: keypad_info(UI_LINK_CONNECTING); break;
    case HOST_LINK_KEYPAD_ONLINE: keypad_info(UI_LINK_ONLINE); break;
    case HOST_LINK_KEYPAD_LOST:
        keypad_info(UI_LINK_LOST);
        D.info.since_epoch_ms = now_ms() - 6LL * 60000LL;
        break;
    case HOST_LINK_KEYPAD_REMOVED:
        keypad_info(UI_LINK_NOT_PAIRED);
        D.info.removed = true;
        D.info.device = 0;
        break;
    default: master_info(); break; /* off, fresh, the alarm panel alone */
    }
    /* The device data: coming while connected; its last before the alarm panel went silent; none
     * before the list is in, or once forgotten (BL-216). */
    if (what == HOST_LINK_KEYPAD_ONLINE) {
        D.dev_heard = now_ms();
    } else if (what == HOST_LINK_KEYPAD_LOST) {
        D.dev_heard = D.info.since_epoch_ms;
    } else {
        D.dev_heard = 0;
        memset(D.ovl, 0, sizeof(D.ovl));
    }
}

/* The lowest number nobody holds (LINK-001), 0 when all eight are taken. */
static uint8_t free_number(void)
{
    for (uint8_t d = 1; d <= KEYPADS; d++) {
        bool used = (D.held & (1u << (d - 1u))) != 0u;
        for (size_t i = 0; i < D.n; i++) {
            used = used || D.kps[i].device == d;
        }
        if (!used) {
            return d;
        }
    }
    return 0;
}

/* Both answered on the alarm panel: the keypad takes the lowest number free, partition 1, and is
 * supervised once it is heard (LINK-016: nothing is stored before both confirmed). */
static void master_done(void)
{
    const uint8_t d = free_number();
    if (d == 0u) {
        D.pair.state = UI_LINK_PAIR_FAILED;
        D.pair.fail = UI_LINK_FAIL_FULL;
        return;
    }
    add_keypad(d, "", false, false, 0);
    D.pair.state = UI_LINK_PAIR_DONE;
    D.pair.device = d;
    master_info();
}

/* The setup's pairing on the alarm panel's first start is done (Option C): link_info carries what
 * the alarm panel gave, its home partition by name for the last step (plink_backend.c). */
static void setup_paired(void)
{
    if (D.info.role == UI_LINK_ROLE_MASTER) {
        D.info.device = D.pair.device;
        D.info.home = 1;
        snprintf(D.info.master_name, sizeof(D.info.master_name), "Harbor House");
        snprintf(D.info.master_addr, sizeof(D.info.master_addr), "192.0.2.42");
        snprintf(D.info.home_name, sizeof(D.info.home_name), "Home");
    }
}

void host_link_demo_advance(uint32_t ms)
{
    if (D.info.selected != 0u && !D.info.return_never && now_ms() >= D.selection_until)
        D.info.selected = 0u;
    if (!D.on) {
        D.ms = 0;
        return;
    }
    /* A keypad's device data keeps coming while connected; its unanswered actions are given up
     * 3 s after they went (BL-216). */
    if (D.info.role == UI_LINK_ROLE_KEYPAD && D.info.state == UI_LINK_ONLINE && !D.dev_stopped) {
        D.dev_heard = now_ms();
    }
    for (size_t i = 0; i < DEV_ACTS; i++) {
        if (D.ovl[i].used && !D.ovl[i].given_up && now_ms() - D.ovl[i].at >= DEV_GIVE_UP) {
            D.ovl[i].given_up = true;
        }
    }
    D.ms += ms;
    while (D.ms >= 1000u) {
        D.ms -= 1000u;
        for (size_t i = 0; i < D.n; i++) {
            if (D.kps[i].waiting) { /* a keypad just added connects within a second or two */
                D.kps[i].waiting = false;
                D.kps[i].connected = true;
                D.kps[i].heard_epoch_ms = now_ms();
            }
        }
        if (!D.pairing || D.pair.state != UI_LINK_PAIR_OPEN) {
            continue;
        }
        if (D.looking) {
            D.pair.left_s++; /* a keypad: looking for this long */
        } else if (D.pair.left_s > 0u && --D.pair.left_s == 0u) {
            D.pair.state = UI_LINK_PAIR_FAILED; /* the window ran out */
            D.pair.fail = UI_LINK_FAIL_TIMEOUT;
        }
    }
}

static bool be_info(void *ctx, ui_link_info_t *out)
{
    (void)ctx;
    if (!D.on) {
        return false;
    }
    *out = D.info;
    return true;
}

static bool be_pair_status(void *ctx, ui_link_pair_t *out)
{
    (void)ctx;
    if (!D.on || !D.pairing) {
        return false;
    }
    *out = D.pair;
    return true;
}

static size_t be_keypads(void *ctx, ui_link_keypad_t *out, size_t max)
{
    (void)ctx;
    if (!D.on || D.info.role != UI_LINK_ROLE_MASTER) {
        return 0;
    }
    const size_t n = D.n < max ? D.n : max;
    memcpy(out, D.kps, n * sizeof(D.kps[0]));
    return n;
}

static ui_link_keypad_t *find(uint8_t device)
{
    for (size_t i = 0; i < D.n; i++) {
        if (D.kps[i].device == device) {
            return &D.kps[i];
        }
    }
    return NULL;
}

/* A keypad looks for an alarm panel open for pairing (a person asked, LINK-016). */
static void keypad_looks(void)
{
    memset(&D.pair, 0, sizeof(D.pair));
    D.pair.state = UI_LINK_PAIR_OPEN;
    D.pairing = true;
    D.looking = true;
    D.master_allowed = false;
    D.keypad_yes = false;
    D.ms = 0;
}

static uint8_t master_request(const ui_link_req_t *req)
{
    const bool allowed = D.allowed == NULL || D.allowed();
    switch ((ui_link_request_t)req->kind) {
    case UI_LINK_REQ_PAIR_OPEN:
        if (!allowed) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        if (free_number() == 0u) {
            return UI_LINK_REQ_FULL;
        }
        memset(&D.pair, 0, sizeof(D.pair));
        D.pair.state = UI_LINK_PAIR_OPEN;
        D.pair.left_s = WINDOW_S;
        D.pairing = true;
        D.looking = false;
        D.master_allowed = false;
        D.keypad_yes = false;
        D.ms = 0;
        return UI_LINK_REQ_OK;
    case UI_LINK_REQ_PAIR_DECIDE:
        if (!D.pairing ||
            (D.pair.state != UI_LINK_PAIR_COMPARE && D.pair.state != UI_LINK_PAIR_WORKING)) {
            return UI_LINK_REQ_BUSY;
        }
        if (!req->allow) {
            D.pair.state = UI_LINK_PAIR_FAILED; /* ends it on both screens, a failed attempt */
            D.pair.fail = UI_LINK_FAIL_REFUSED;
            return UI_LINK_REQ_OK;
        }
        D.master_allowed = true;
        if (D.keypad_yes) {
            master_done();
        } else {
            D.pair.state = UI_LINK_PAIR_WAITING;
        }
        return UI_LINK_REQ_OK;
    case UI_LINK_REQ_PAIR_STOP:
        D.pairing = false;
        memset(&D.pair, 0, sizeof(D.pair));
        return UI_LINK_REQ_OK;
    case UI_LINK_REQ_KEYPAD_SET: {
        if (!allowed) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        ui_link_keypad_t *k = find(req->device);
        if (k == NULL || req->home < 1u || req->home > 8u) {
            return UI_LINK_REQ_REFUSED;
        }
        snprintf(k->name, sizeof(k->name), "%s", req->name);
        k->home = req->home;
        k->supervision_s = req->supervision_s;
        return UI_LINK_REQ_OK;
    }
    case UI_LINK_REQ_PARTITION_POLICY: {
        ui_link_keypad_t *k = find(req->device);
        if (!allowed) return UI_LINK_REQ_NOT_ALLOWED;
        if (k == NULL) return UI_LINK_REQ_REFUSED;
        k->switch_allowed = req->switch_allowed;
        k->return_never = req->return_never;
        k->return_s = req->return_s;
        return UI_LINK_REQ_OK;
    }
    case UI_LINK_REQ_KEYPAD_REMOVE: {
        if (!allowed) {
            return UI_LINK_REQ_NOT_ALLOWED;
        }
        ui_link_keypad_t *k = find(req->device);
        if (k == NULL) {
            return UI_LINK_REQ_REFUSED;
        }
        const size_t i = (size_t)(k - D.kps);
        memmove(&D.kps[i], &D.kps[i + 1u], (D.n - i - 1u) * sizeof(D.kps[0]));
        D.n--;
        master_info();
        return UI_LINK_REQ_OK;
    }
    case UI_LINK_REQ_KP_PAIR_START:
    case UI_LINK_REQ_KP_PAIR_RETRY:
        /* a panel on its first start, becoming a keypad: it isn't one until Done */
        keypad_looks();
        return UI_LINK_REQ_OK;
    case UI_LINK_REQ_KP_PAIR_STOP:
        /* the setup left the keypad branch: nothing of its pairing stays (ksvc_setup_end) */
        D.pairing = false;
        memset(&D.pair, 0, sizeof(D.pair));
        master_info();
        return UI_LINK_REQ_OK;
    default: break;
    }
    return UI_LINK_REQ_REFUSED;
}

static uint8_t keypad_request(const ui_link_req_t *req)
{
    switch ((ui_link_request_t)req->kind) {
    case UI_LINK_REQ_KP_PAIR_START:
    case UI_LINK_REQ_KP_PAIR_RETRY: keypad_looks(); return UI_LINK_REQ_OK;
    case UI_LINK_REQ_KP_PAIR_STOP:
        D.pairing = false;
        memset(&D.pair, 0, sizeof(D.pair));
        return UI_LINK_REQ_OK;
    default: break;
    }
    return UI_LINK_REQ_REFUSED;
}

static uint8_t be_request(void *ctx, const ui_link_req_t *req)
{
    (void)ctx;
    D.last = *req;
    if (!D.on) {
        return UI_LINK_REQ_REFUSED;
    }
    switch ((ui_link_request_t)req->kind) {
    case UI_LINK_REQ_KP_PAIR_CONFIRM:
        if (!D.pairing || D.pair.state != UI_LINK_PAIR_COMPARE) {
            return UI_LINK_REQ_BUSY;
        }
        if (!req->allow) {
            D.pair.state = UI_LINK_PAIR_FAILED; /* No on either screen: nothing stored */
            D.pair.fail = UI_LINK_FAIL_MISMATCH;
            return UI_LINK_REQ_OK;
        }
        D.keypad_yes = true;
        if (D.master_allowed) {
            D.pair.state = UI_LINK_PAIR_DONE;
            D.pair.device = 2;
            setup_paired();
        } else {
            D.pair.state = UI_LINK_PAIR_WAITING;
        }
        return UI_LINK_REQ_OK;
    case UI_LINK_REQ_KP_WIFI:
        if (req->allow && D.pair.state != UI_LINK_PAIR_DONE) return UI_LINK_REQ_REFUSED;
        D.pair.wifi_setup = req->allow ? 1u : 0u;
        return UI_LINK_REQ_OK;
    case UI_LINK_REQ_KP_DONE:
        if (!D.pairing || D.pair.state != UI_LINK_PAIR_DONE) {
            return UI_LINK_REQ_REFUSED;
        }
        keypad_info(UI_LINK_CONNECTING); /* the role is stored; the panel restarts as a keypad */
        D.info.device = D.pair.device;
        D.pairing = false;
        return UI_LINK_REQ_OK;
    default: break;
    }
    return D.info.role == UI_LINK_ROLE_MASTER ? master_request(req) : keypad_request(req);
}

/* ---- Smart Home on a keypad (BL-216) -------------------------------------------------------- */

static bool keypad_role(void)
{
    return D.on && D.info.role == UI_LINK_ROLE_KEYPAD;
}

/* The alarm panel's list is in: from the first sync, kept through a loss, gone once forgotten. */
static bool dev_listed(void)
{
    return keypad_role() && (D.info.state == UI_LINK_ONLINE || D.info.state == UI_LINK_LOST);
}

/* LINK-046: connected, with device data within 30 s. */
static bool dev_fresh(void)
{
    return dev_listed() && D.info.state == UI_LINK_ONLINE && D.dev_heard != 0 &&
           now_ms() - D.dev_heard < DEV_STALE_MS;
}

static dev_ovl_t *ovl_of(const char *ref)
{
    for (size_t i = 0; i < DEV_ACTS; i++) {
        if (D.ovl[i].used && strcmp(D.ovl[i].was.ref, ref) == 0) {
            return &D.ovl[i];
        }
    }
    return NULL;
}

/* plink_backend.c's kp_dev_list: the alarm panel's devices as the keypad's mirror shows them -
 * its own action over a device until that device's next record (plink_keypad_dev_view), and every
 * device offline since the last device data while that isn't fresh. */
static size_t kp_dev_list(void *ctx, ui_dev_t *out, size_t max)
{
    if (!keypad_role()) {
        return P.list(ctx, out, max);
    }
    if (!dev_listed()) {
        return 0; /* no list yet: no Smart Home on the rail */
    }
    const size_t n = P.list(ctx, out, max);
    const bool fresh = dev_fresh();
    for (size_t i = 0; i < n; i++) {
        ui_dev_t *d = &out[i];
        dev_ovl_t *o = ovl_of(d->ref);
        if (o != NULL && memcmp(&o->was, d, sizeof(*d)) != 0) {
            memset(o, 0, sizeof(*o)); /* a later record: it shows what the alarm panel says */
        } else if (o != NULL && d->status != UI_DEV_OFFLINE) {
            d->status = o->given_up ? UI_DEV_FAILED : UI_DEV_PENDING;
            d->action = o->action;
            d->unsure = o->given_up ? 1u : 0u;
        }
        if (!fresh) {
            if (d->status != UI_DEV_OFFLINE) {
                d->since_ms = D.dev_heard; /* since the last device data */
            }
            d->status = UI_DEV_OFFLINE;
            d->action = ACT_NONE;
            d->unsure = 0u;
        }
    }
    return n;
}

/* plink_backend.c's kp_dev_act: steps 1-4 answered at once from the screen's copy, then the
 * alarm panel's judge - or, its answers lost, the device pending until the give-up. */
static ui_dev_result_t kp_dev_act(void *ctx, const char *ref, uint8_t action, int32_t value,
                                  const char *code, alarm_result_t *code_result)
{
    if (!keypad_role()) {
        return P.act(ctx, ref, action, value, code, code_result);
    }
    if (!dev_fresh() || ref == NULL) {
        return UI_DEV_UNREACHABLE; /* nothing sent */
    }
    const size_t n = kp_dev_list(ctx, s_rows, UI_DEVL_MAX);
    const ui_dev_t *d = NULL;
    for (size_t i = 0; i < n && d == NULL; i++) {
        d = strcmp(s_rows[i].ref, ref) == 0 ? &s_rows[i] : NULL;
    }
    if (d == NULL) {
        return UI_DEV_REFUSED;
    }
    if (d->status == UI_DEV_OFFLINE) {
        return UI_DEV_UNREACHABLE;
    }
    if (d->source == (uint8_t)DSRC_ZWAVE ||
        !hc_action_valid((dev_kind_t)d->kind, (dev_action_t)action) ||
        (action == (uint8_t)ACT_SET_MODE && ui_dev_mode_bit(value) == 0u)) {
        return UI_DEV_REFUSED;
    }
    const bool coded = code != NULL && code[0] != '\0';
    if (!coded && hc_needs_code((dev_kind_t)d->kind, (dev_action_t)action, value)) {
        return UI_DEV_NEED_CODE; /* the pad opens with no round trip */
    }
    dev_ovl_t *o = ovl_of(ref);
    if (o != NULL && !o->given_up) {
        return UI_DEV_BUSY; /* one at a time on a device */
    }
    if (o != NULL) {
        memset(o, 0, sizeof(*o)); /* the next action clears the last one's words */
    }
    if (!D.dev_unanswered) {
        return P.act(ctx, ref, action, value, code, code_result);
    }
    for (size_t i = 0; i < DEV_ACTS; i++) {
        if (!D.ovl[i].used) {
            const size_t k = (size_t)(d - s_rows);
            (void)P.list(ctx, s_rows, UI_DEVL_MAX); /* the alarm panel's own row, untouched */
            D.ovl[i].used = true;
            D.ovl[i].action = action;
            D.ovl[i].at = now_ms();
            D.ovl[i].was = s_rows[k];
            return UI_DEV_OK; /* the screen stopped waiting: pending until the give-up */
        }
    }
    return UI_DEV_BUSY;
}

void host_link_demo_devices_stop(bool on)
{
    D.dev_stopped = on;
    if (!on && D.info.state == UI_LINK_ONLINE) {
        D.dev_heard = now_ms();
    }
}

void host_link_demo_devices_unanswered(bool on)
{
    D.dev_unanswered = on;
}

static alarm_result_t partition_select(void *ctx, uint8_t partition, bool activity)
{
    (void)ctx;
    if (D.info.state != UI_LINK_ONLINE || partition < 1u || partition > ALARM_MAX_PARTITIONS)
        return ALARM_ERR_UNAVAILABLE;
    const uint8_t selected = D.info.selected != 0u ? D.info.selected : D.info.home;
    if ((activity && partition != selected) ||
        (!activity && partition != D.info.home && !D.info.switch_allowed))
        return ALARM_ERR_NOT_AUTHORIZED;
    D.info.selected = partition == D.info.home ? 0u : partition;
    D.selection_until = now_ms() + (D.info.return_s != 0u ? D.info.return_s : 30u) * 1000;
    return ALARM_OK;
}

void host_link_demo_partition_policy(bool allowed, bool never, uint16_t seconds)
{
    D.info.switch_allowed = allowed;
    D.info.return_never = never;
    D.info.return_s = seconds;
    D.info.selected = 0u;
}

void host_link_demo_remote(uint32_t areas, bool installer, uint8_t holder)
{
    D.info.remote_areas = areas;
    D.info.remote_installer = installer;
    D.info.remote_holder = holder;
    D.info.remote_gen++;
}

void host_link_demo_backend(ui_backend_t *out)
{
    out->link_info = be_info;
    out->link_pair_status = be_pair_status;
    out->link_keypads = be_keypads;
    out->link_request = be_request;
    out->partition_select = partition_select;
    /* Smart Home: the alarm panel's own, through the link on a keypad (BL-216) */
    if (out->dev_list != NULL && out->dev_act != NULL && out->dev_list != kp_dev_list) {
        P.list = out->dev_list;
        P.act = out->dev_act;
        out->dev_list = kp_dev_list;
        out->dev_act = kp_dev_act;
    }
}

void host_link_demo_keypad_asks(void)
{
    if (D.pairing && D.pair.state == UI_LINK_PAIR_OPEN) {
        D.pair.state = UI_LINK_PAIR_COMPARE;
        D.pair.attempts = 1;
        snprintf(D.pair.code, sizeof(D.pair.code), PAIR_CODE);
        snprintf(D.pair.peer_addr, sizeof(D.pair.peer_addr), "192.0.2.57");
    }
}

void host_link_demo_keypad_confirms(void)
{
    if (!D.pairing) {
        return;
    }
    D.keypad_yes = true;
    if (D.master_allowed && D.pair.state == UI_LINK_PAIR_WAITING) {
        master_done();
    }
}

void host_link_demo_master_found(void)
{
    if (D.pairing && D.pair.state == UI_LINK_PAIR_OPEN) {
        D.pair.state = UI_LINK_PAIR_COMPARE;
        D.pair.attempts = 1;
        snprintf(D.pair.code, sizeof(D.pair.code), PAIR_CODE);
        snprintf(D.pair.peer_name, sizeof(D.pair.peer_name), "Harbor House");
        snprintf(D.pair.peer_addr, sizeof(D.pair.peer_addr), "192.0.2.42");
    }
}

void host_link_demo_master_allows(void)
{
    if (!D.pairing) {
        return;
    }
    D.master_allowed = true;
    if (D.keypad_yes && D.pair.state == UI_LINK_PAIR_WAITING) {
        D.pair.state = UI_LINK_PAIR_DONE;
        D.pair.device = 2;
        setup_paired();
    }
}

void host_link_demo_pair_fail(uint8_t fail)
{
    if (D.pairing) {
        D.pair.state = UI_LINK_PAIR_FAILED;
        D.pair.fail = fail;
    }
}

void host_link_demo_numbers_held(uint16_t mask)
{
    D.held = mask;
}

void host_link_demo_panic_pending(bool on)
{
    D.info.panic_pending = on;
}

void host_link_demo_maintenance(bool on)
{
    D.info.maintenance = on;
}

void host_link_demo_local_alarm(bool on)
{
    D.info.local_alarm = on;
}

void host_link_demo_needs_update(bool on)
{
    D.info.needs_update = on;
}

void host_link_demo_home(uint8_t home)
{
    D.info.home = home;
    D.info.home_gen++;
}

void host_link_demo_keypad_fw(uint8_t device, const char *fw, uint8_t state, uint8_t percent)
{
    ui_link_keypad_t *k = find(device);
    if (k == NULL) {
        return;
    }
    const bool was_restarting = k->fw_state == UI_LINK_FW_RESTARTING;
    if (fw != NULL) {
        snprintf(k->fw, sizeof(k->fw), "%s", fw);
    }
    k->fw_state = state;
    k->fw_percent = percent > 100u ? 100u : percent;
    if (state == UI_LINK_FW_RESTARTING) {
        k->connected = false; /* off the link while it restarts; its trouble is held */
        k->down = false;
        k->waiting = false;
    } else if (was_restarting) {
        k->connected = true;
        k->heard_epoch_ms = now_ms();
    }
    master_info();
}

void host_link_demo_fw_follows(bool follows, bool from_master)
{
    D.info.fw_follows = follows;
    D.info.fw_from_master = from_master;
}

void host_link_demo_last_request(ui_link_req_t *out)
{
    *out = D.last;
}

size_t host_link_demo_keypads(void)
{
    return D.n;
}

bool host_link_demo_keypad(uint8_t device, ui_link_keypad_t *out)
{
    const ui_link_keypad_t *k = find(device);
    if (k == NULL) {
        return false;
    }
    *out = *k;
    return true;
}

void host_link_demo_wifi_setup(uint8_t state, bool online)
{
    D.pair.wifi_setup = state;
    D.pair.online = online;
}

void host_link_demo_unpaired(void)
{
    D.pairing = false;
    memset(&D.pair, 0, sizeof D.pair);
}

void host_link_demo_resume_pairing(bool published)
{
    keypad_info(UI_LINK_CONNECTING);
    D.pairing = published;
    D.pair.state = UI_LINK_PAIR_DONE;
    D.pair.device = 2;
    D.pair.wifi_setup = 0;
    D.pair.online = false;
}
