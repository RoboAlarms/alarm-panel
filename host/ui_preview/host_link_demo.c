/*
 * host_link_demo.c - the demo panel-link backend (host_link_demo.h): ui_backend_t's link_* over a
 * small model of Harbor House and its keypads on Wi-Fi, for the PC preview's walk and the
 * simulator. Fictional data (RFC 5737 addresses).
 */
#include "host_link_demo.h"

#include <stdio.h>
#include <string.h>

#define WINDOW_S  120u /* the alarm panel's pairing window (LINK-016) */
#define KEYPADS   8u   /* numbers 1-8 (LINK-001) */
#define PAIR_CODE "588460"

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
    if (!D.on) {
        D.ms = 0;
        return;
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

void host_link_demo_backend(ui_backend_t *out)
{
    out->link_info = be_info;
    out->link_pair_status = be_pair_status;
    out->link_keypads = be_keypads;
    out->link_request = be_request;
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
