/*
 * host_zigbee_real.c - the panel's real Zigbee on the PC (host_zigbee_real.h): zbz_panel, its zone
 * source and the fake radio, wired as zigbee_service wires them on a panel and as
 * components/core/zigbee_zones/test/test_zbz_replay.c drives them, with the host's clock.
 */
#include "host_zigbee_real.h"

#include <stdio.h>
#include <string.h>

#include "zbz_fake_radio.h"

#define KV_MAX     (ZBZ_DEV_MAX + 8u)
#define FQ_MAX     8u
#define JOIN_FIRST 3000u  /* the first device presses its button this long after the window opens */
#define JOIN_GAP   3000u  /* ... the second this long after it */
#define SETUP_WAKE 30000u /* a sleepy device listens this long after its own frame */
#define RESTART_MS 2000u  /* the radio's restart (RESTART, RETRY) */
#define BATTERY_MS 5000u  /* a battery device's first battery report, this long after it joined */
#define BATTERY_PCT 92u
/* Each setup request's answer leaves this much later, so a device's setup takes a few seconds and
 * the add's page shows its steps go by, as a real device's would (ZB-027) */
#define SETUP_STEP_MS 250u

/* A store slot holds the largest value zbz_panel hands out, as an NVS blob would. */
#define KV_MAX2(a, b) ((a) > (b) ? (a) : (b))
#define KV_BLOB_MAX                                                                                \
    KV_MAX2(KV_MAX2(ZBZ_RECORD_MAX, ZBZ_CODES_BLOB_MAX),                                           \
            KV_MAX2(KV_MAX2(ZBA_BLOB_MAX, ZBW_BLOB_MAX), ZBZ_CFG_BLOB))
_Static_assert(KV_BLOB_MAX >= ZBZ_RECORD_MAX && KV_BLOB_MAX >= ZBZ_CODES_BLOB_MAX &&
                   KV_BLOB_MAX >= ZBA_BLOB_MAX && KV_BLOB_MAX >= ZBW_BLOB_MAX &&
                   KV_BLOB_MAX >= ZBZ_CFG_BLOB,
               "a store slot holds every blob zbz_panel writes");

/* The virtual devices: the signature each is built from. */
static const char *const SIGNATURE[HOST_ZBR_COUNT] = {
    [HOST_ZBR_DOOR] = "sonoff_snzb_04pr2",
    [HOST_ZBR_MOTION] = "centralite_3326l",
    [HOST_ZBR_SMOKE] = "heiman_smoke_em",
    /* no water sensor among the signatures: a contact of another make with the IAS zone type of a
     * water sensor and a model the model table doesn't know */
    [HOST_ZBR_WATER] = "third_reality_3rds17bz",
    [HOST_ZBR_SIREN] = "frient_sirzb_111",
    [HOST_ZBR_KEYPAD] = "frient_kepzb_110",
    [HOST_ZBR_FOB] = "heiman_hs1rc",
};

typedef struct {
    bool used;
    char key[ZBZ_KEY_LEN];
    uint8_t buf[KV_BLOB_MAX];
    size_t len;
} kv_t;

static struct {
    bool on;
    bool walk; /* zbz_panel told a walk test runs */
    host_zbr_port_t port;
    zone_hub_t *hub;
    uint64_t now;
    zbf_radio_t radio;
    zsrc_zb_t src;
    zsrc_zb_radio_t hooks;
    zbz_panel_t panel;
    zbf_dev_t *dev[HOST_ZBR_COUNT];
    kv_t kv[KV_MAX];              /* the simulator's NVS "zbpanel" */
    zba_post_t fq[FQ_MAX];        /* the alarm task's queue */
    unsigned fqn;
    zbz_radio_info_t told;
    bool told_valid;
    bool unplugged;
    bool store_failed;            /* the last write_back kept something back */
    unsigned ace_answered;        /* answers zbz_panel_ace_result took (in time) */
    uint64_t restart_until;       /* the radio restarts until then; 0 not */
    bool window_was_open;
    uint64_t battery_at[HOST_ZBR_COUNT]; /* its first battery report is due then; 0 none */
    struct {
        uint8_t which;            /* host_zbr_dev_t */
        uint64_t at;              /* 0: none */
    } join[2];
} S;

/* ---- the store ------------------------------------------------------------------------------- */

static kv_t *kv_find(const char *key)
{
    for (size_t i = 0; i < KV_MAX; i++) {
        if (S.kv[i].used && strcmp(S.kv[i].key, key) == 0) {
            return &S.kv[i];
        }
    }
    return NULL;
}

/* false when it can't be kept (every slot used): the caller asks for it again (zbz_panel_redo),
 * as zigbee_service's persist() does after a failed NVS write. */
static bool kv_put(const char *key, const uint8_t *buf, size_t len)
{
    kv_t *e = kv_find(key);
    for (size_t i = 0; e == NULL && i < KV_MAX; i++) {
        if (!S.kv[i].used) {
            e = &S.kv[i];
        }
    }
    if (e == NULL || len > sizeof(e->buf)) {
        return false;
    }
    e->used = true;
    snprintf(e->key, sizeof(e->key), "%s", key);
    memcpy(e->buf, buf, len);
    e->len = len;
    return true;
}

static void kv_erase(const char *key)
{
    kv_t *e = kv_find(key);
    if (e != NULL) {
        memset(e, 0, sizeof(*e));
    }
}

static size_t st_read(void *ctx, const char *key, uint8_t *buf, size_t cap)
{
    (void)ctx;
    const kv_t *e = kv_find(key);
    if (e == NULL) {
        return 0u;
    }
    if (e->len <= cap) {
        memcpy(buf, e->buf, e->len);
    }
    return e->len;
}

static const zbz_store_port_t STORE = {.ctx = NULL, .read = st_read};

/* What zbz_panel hands out, into the store (the firmware's persist after unlocking). */
static void write_back(void)
{
    char key[ZBZ_KEY_LEN];
    uint8_t buf[KV_BLOB_MAX];
    size_t len = 0u;
    uint8_t failed = 0u;
    for (int guard = 0; guard < 200; guard++) {
        const zbz_take_t t = zbz_panel_take_registry(&S.panel, key, buf, sizeof(buf), &len);
        if (t == ZBZ_TAKE_NONE) {
            break;
        }
        if (t == ZBZ_TAKE_WRITE) {
            if (!kv_put(key, buf, len)) {
                failed |= ZBZ_DIRTY_REGISTRY;
            }
        } else {
            kv_erase(key);
        }
    }
    len = zbz_panel_take_codes(&S.panel, buf, sizeof(buf));
    if (len > 0u) {
        if (!kv_put(ZBZ_KEY_CODES, buf, len)) {
            failed |= ZBZ_DIRTY_CODES;
        }
    }
    len = zbz_panel_take_cfg(&S.panel, buf, sizeof(buf));
    if (len > 0u) {
        if (!kv_put(ZBZ_CFG_KEY, buf, len)) {
            failed |= ZBZ_DIRTY_CFG;
        }
    }
    len = zbz_panel_take_wd(&S.panel, buf, sizeof(buf));
    if (len > 0u) {
        if (!kv_put(ZBW_KEY, buf, len)) {
            failed |= ZBZ_DIRTY_WD;
        }
    }
    len = zbz_panel_take_ace(&S.panel, buf, sizeof(buf));
    if (len > 0u) {
        if (!kv_put(ZBA_KEY, buf, len)) {
            failed |= ZBZ_DIRTY_ACE;
        }
    }
    memset(buf, 0, sizeof(buf)); /* the codes' bytes */
    if (failed != 0u) {
        zbz_panel_redo(&S.panel, failed);
        if (!S.store_failed) {
            fprintf(stderr, "zigbee: the store is full (0x%02x): tried again\n", (unsigned)failed);
        }
    }
    S.store_failed = failed != 0u;
}

/* ---- the panel's ports ------------------------------------------------------------------------ */

static void f_report(void *ctx, const zsrc_zb_report_t *r, uint64_t now)
{
    (void)ctx;
    zbz_panel_on_report(&S.panel, now, r);
}

static bool p_binding(void *ctx, uint16_t zone, zone_hub_binding_t *out)
{
    (void)ctx;
    return S.hub != NULL && zone_hub_get(S.hub, zone, out);
}

static bool p_rebind(void *ctx, uint16_t zone, const zone_hub_binding_t *b)
{
    (void)ctx;
    if (S.hub != NULL && S.port.bind != NULL) {
        return S.port.bind(S.port.ctx, zone, b) == ZONE_HUB_OK;
    }
    return S.hub != NULL && zone_hub_bind(S.hub, zone, b, S.now) == ZONE_HUB_OK;
}

static uint32_t p_wall(void *ctx)
{
    (void)ctx;
    return S.port.wall_clock != NULL ? S.port.wall_clock(S.port.ctx) : 0u;
}

static uint32_t p_taken(void *ctx)
{
    (void)ctx;
    return S.port.devices_taken != NULL ? S.port.devices_taken(S.port.ctx) : 0u;
}

static bool p_post(void *ctx, const zba_post_t *post)
{
    (void)ctx;
    if (S.fqn >= FQ_MAX || post == NULL) {
        return false;
    }
    S.fq[S.fqn++] = *post;
    return true;
}

static void w_bell(void *ctx, bool active)
{
    (void)ctx;
    if (S.port.bell_trouble != NULL) {
        S.port.bell_trouble(S.port.ctx, active);
    }
}

/* A keypad's Default Response: the stack would send it; the fake keypad needs none. */
static bool a_default_rsp(void *ctx, uint64_t ieee, uint8_t ep, uint8_t tsn, uint8_t command,
                          uint8_t status, uint32_t tag)
{
    (void)ctx;
    (void)ieee;
    (void)ep;
    (void)tsn;
    (void)command;
    (void)status;
    (void)tag;
    return true;
}

static const zbz_panel_port_t PORT = {.ctx = NULL,
                                      .binding = p_binding,
                                      .rebind = p_rebind,
                                      .wall_clock = p_wall,
                                      .changed = NULL,
                                      .devices_taken = p_taken,
                                      .ace_post = p_post};

static void panel_start(void)
{
    const zbw_port_t wd = {.ctx = NULL, .send = NULL, .bell_trouble = w_bell, .checkin = NULL};
    const zba_port_t ace = {.ctx = NULL, .send = NULL, .default_rsp = a_default_rsp};
    zbz_panel_init(&S.panel, &PORT, &S.src, &S.hooks, &wd, &ace);
    (void)zbz_panel_load(&S.panel, &STORE);
    S.told_valid = false;
}

/* ---- the radio as the service sees it ---------------------------------------------------------- */

static void publish_info(void)
{
    zbz_radio_info_t info;
    memset(&info, 0, sizeof(info));
    if (S.unplugged) {
        info.state = (uint8_t)UI_ZB_RADIO_NOT_CONNECTED;
    } else if (S.restart_until != 0u) {
        info.state = (uint8_t)UI_ZB_RADIO_STARTING;
    } else {
        info.state = (uint8_t)UI_ZB_RADIO_WORKING;
        snprintf(info.version, sizeof(info.version), "%s", "1.0.0 (simulated)");
        info.jammed = zsrc_zb_jammed(&S.src);
        info.noise_known = S.src.link_up && S.src.energy_at != 0u;
        info.noise_dbm = S.src.energy_dbm;
        if (S.src.learning && S.src.learn_until > S.now) {
            info.learn_s = (uint16_t)((S.src.learn_until - S.now + 999u) / 1000u);
        }
    }
    if (!S.told_valid || memcmp(&info, &S.told, sizeof(info)) != 0) {
        S.told = info;
        S.told_valid = true;
        zbz_panel_radio(&S.panel, S.now, &info);
    }
}

/* ---- the virtual devices ----------------------------------------------------------------------- */

static void add_devices(void)
{
    for (size_t i = 0; i < HOST_ZBR_COUNT; i++) {
        zbf_dev_t *d = zbf_add(&S.radio, SIGNATURE[i]);
        S.dev[i] = d;
        if (d == NULL) {
            fprintf(stderr, "zigbee: no signature %s\n", SIGNATURE[i]);
            continue;
        }
        if (d->sleepy) {
            zbf_set_sleepy(d, true, SETUP_WAKE);
        }
        zbf_set_link(d, 172u, -58);
        const zbf_fault_t slow = {.delay_ms = SETUP_STEP_MS};
        for (uint8_t step = (uint8_t)ZSRC_ZB_STEP_NODE_DESC; step < (uint8_t)ZSRC_ZB_STEP_DONE;
             step++) {
            zbf_fault(d, step, &slow);
        }
    }
    zbf_dev_t *w = S.dev[HOST_ZBR_WATER];
    if (w != NULL) {
        for (size_t e = 0; e < w->ep_count; e++) {
            if (w->ep[e].zone_type != ZSRC_ZB_ZONE_TYPE_NONE) {
                w->ep[e].zone_type = ZSRC_ZB_ZONE_TYPE_WATER;
            }
        }
        snprintf(w->model, sizeof(w->model), "%s", "3RWS18BZ");
    }
}

static bool window_open(void)
{
    return !S.unplugged && S.restart_until == 0u && S.radio.join_until > S.now;
}

static host_zbr_dev_t next_out(host_zbr_dev_t after)
{
    for (size_t i = 0; i < HOST_ZBR_COUNT; i++) {
        if (S.dev[i] != NULL && !S.dev[i]->joined && (int)i != (int)after) {
            return (host_zbr_dev_t)i;
        }
    }
    return HOST_ZBR_COUNT;
}

/* The window opened: two devices press their buttons, the one an install code waits for first. */
static void auto_joins(void)
{
    const bool open = window_open();
    if (open && !S.window_was_open) {
        host_zbr_dev_t first = HOST_ZBR_COUNT;
        for (size_t i = 0; i < HOST_ZBR_COUNT && S.panel.op_ieee != 0u; i++) {
            if (S.dev[i] != NULL && !S.dev[i]->joined && S.dev[i]->ieee == S.panel.op_ieee) {
                first = (host_zbr_dev_t)i;
            }
        }
        const bool by_code = first != HOST_ZBR_COUNT;
        if (!by_code) {
            first = next_out(HOST_ZBR_COUNT);
        }
        /* an install-code add listens for its own device only */
        const host_zbr_dev_t second = by_code ? HOST_ZBR_COUNT : next_out(first);
        S.join[0].which = (uint8_t)first;
        S.join[0].at = first != HOST_ZBR_COUNT ? S.now + JOIN_FIRST : 0u;
        S.join[1].which = (uint8_t)second;
        S.join[1].at = second != HOST_ZBR_COUNT ? S.now + JOIN_FIRST + JOIN_GAP : 0u;
    }
    S.window_was_open = open;
    for (size_t j = 0; j < 2u; j++) {
        if (S.join[j].at != 0u && S.join[j].at <= S.now) {
            S.join[j].at = 0u;
            if (open) {
                (void)host_zigbee_real_press((host_zbr_dev_t)S.join[j].which);
            }
        }
    }
}

/* A battery device reports its battery once it is set up, as one does after its reporting is
 * configured (the page then shows "Battery 92 %"). */
static void battery_step(void)
{
    for (size_t i = 0; i < HOST_ZBR_COUNT; i++) {
        if (S.battery_at[i] != 0u && S.battery_at[i] <= S.now) {
            S.battery_at[i] = 0u;
            (void)zbf_set_battery(&S.radio, S.dev[i], BATTERY_PCT);
        }
    }
}

static void restart_step(void)
{
    if (S.restart_until != 0u && S.restart_until <= S.now) {
        S.restart_until = 0u;
        if (!S.unplugged) {
            zbf_unplug(&S.radio, false);
        }
    }
}

/* ---- the owner's work after the panel's: store, log, the alarm task's queue -------------------- */

static void flush(void)
{
    write_back();
    zbz_log_t e;
    while (zbz_panel_take_log(&S.panel, &e)) {
        if (S.port.log != NULL) {
            S.port.log(S.port.ctx, &e);
        }
    }
    for (int guard = 0; guard < 4 && S.fqn > 0u; guard++) {
        zba_post_t q[FQ_MAX];
        const unsigned n = S.fqn;
        memcpy(q, S.fq, sizeof(q[0]) * n);
        memset(S.fq, 0, sizeof(S.fq)); /* the codes */
        S.fqn = 0u;
        for (unsigned i = 0; i < n; i++) {
            alarm_cmd_result_t detail;
            memset(&detail, 0, sizeof(detail));
            const alarm_result_t r = S.port.ace_run != NULL
                                         ? S.port.ace_run(S.port.ctx, &q[i], &detail)
                                         : ALARM_ERR_NOT_AUTHORIZED;
            if (zbz_panel_ace_result(&S.panel, S.now, q[i].ticket, r, &detail)) {
                S.ace_answered++;
            }
        }
        memset(q, 0, sizeof(q));
    }
}

static uint64_t earliest(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

uint64_t host_zigbee_real_next_due(void)
{
    if (!S.on) {
        return UINT64_MAX;
    }
    uint64_t t = earliest(zbf_next_due(&S.radio), zbz_panel_next_deadline(&S.panel));
    for (size_t j = 0; j < 2u; j++) {
        if (S.join[j].at != 0u) {
            t = earliest(t, S.join[j].at);
        }
    }
    if (S.restart_until != 0u) {
        t = earliest(t, S.restart_until);
    }
    for (size_t i = 0; i < HOST_ZBR_COUNT; i++) {
        if (S.battery_at[i] != 0u) {
            t = earliest(t, S.battery_at[i]);
        }
    }
    if (S.src.learning) { /* the join window's countdown, each second */
        t = earliest(t, S.now + 1000u - S.now % 1000u);
    }
    return t;
}

void host_zigbee_real_run(uint64_t now)
{
    if (!S.on) {
        return;
    }
    for (unsigned guard = 0; guard < 1000000u; guard++) {
        restart_step();
        battery_step();
        zbz_panel_poll(&S.panel, S.now);
        publish_info();
        auto_joins();
        if (S.now >= now) {
            break;
        }
        uint64_t t = host_zigbee_real_next_due();
        t = t <= S.now ? S.now + 1u : t;
        t = t < now ? t : now;
        S.now = t;
        zbf_advance(&S.radio, S.now);
    }
    flush();
}

/* ---- life cycle ------------------------------------------------------------------------------- */

void host_zigbee_real_init(zone_hub_t *hub, const zsrc_sink_t *sink, const host_zbr_port_t *port,
                           uint64_t now)
{
    memset(&S, 0, sizeof(S));
    S.on = true;
    S.hub = hub;
    if (port != NULL) {
        S.port = *port;
    }
    S.now = now;
    const zbf_sink_t fs = {.ctx = NULL, .report = f_report, .attr = NULL};
    zbf_init(&S.radio, &S.src, &fs, now);
    S.hooks = zbf_hooks(&S.radio);
    zsrc_zb_init(&S.src, &S.hooks, sink);
    if (hub != NULL) {
        (void)zone_hub_register(hub, zsrc_zb_ops(), &S.src, now);
    }
    add_devices();
    panel_start();
    zbf_up(&S.radio);
    host_zigbee_real_run(now);
}

void host_zigbee_real_restart(uint64_t now)
{
    if (!S.on) {
        return;
    }
    host_zigbee_real_run(now);
    panel_start();
    S.window_was_open = false;
    S.walk = false; /* the new zbz_panel is told again at the next news */
    host_zigbee_real_run(now);
}

void host_zigbee_real_output(uint8_t partition, alarm_output_t output, alarm_pattern_t pattern)
{
    if (S.on) {
        zbz_panel_output(&S.panel, S.now, partition, output, pattern);
    }
}

void host_zigbee_real_partition(uint8_t partition, const zba_view_t *view)
{
    if (S.on) {
        zbz_panel_partition(&S.panel, S.now, partition, view);
    }
}

void host_zigbee_real_walk(bool on, const uint8_t covered[ALARM_ZONE_BITMAP_BYTES])
{
    if (!S.on) {
        return;
    }
    if (on != S.walk) {
        S.walk = on;
        zbz_panel_walk_test(&S.panel, S.now, on);
    }
    zbz_panel_walk_scope(&S.panel, S.now, covered);
}

const zbz_panel_t *host_zigbee_real_panel(void)
{
    return &S.panel;
}

/* ---- the pages' calls ------------------------------------------------------------------------- */

static bool be_zb_status(void *ctx, ui_zb_status_t *out)
{
    (void)ctx;
    if (!S.on) {
        return false;
    }
    zbz_panel_status(&S.panel, S.now, out);
    return true;
}

static size_t be_zb_devices(void *ctx, ui_zb_device_t *out, size_t max)
{
    (void)ctx;
    return S.on ? zbz_panel_devices(&S.panel, S.now, out, max) : 0u;
}

static bool be_zb_device(void *ctx, uint16_t id, ui_zb_device_page_t *out)
{
    (void)ctx;
    return S.on && zbz_panel_device(&S.panel, S.now, id, out);
}

static void be_zb_progress(void *ctx, ui_zb_progress_t *out)
{
    (void)ctx;
    if (!S.on) {
        memset(out, 0, sizeof(*out));
        return;
    }
    zbz_panel_progress(&S.panel, S.now, out);
}

static size_t be_zb_codes(void *ctx, ui_zb_code_t *out, size_t max)
{
    (void)ctx;
    return S.on ? zbz_panel_codes(&S.panel, out, max) : 0u;
}

static ui_zb_req_result_t be_zb_request(void *ctx, const ui_zb_request_t *req)
{
    (void)ctx;
    if (!S.on || req == NULL) {
        return UI_ZB_REQ_NO_RADIO;
    }
    /* ZB-017: the reset's installer code, checked by the engine first (ui_port_esp's order) */
    if (req->kind == (uint8_t)UI_ZB_REQ_RESET) {
        const ui_zb_req_result_t c = S.port.installer_code != NULL
                                         ? S.port.installer_code(S.port.ctx, req->installer_code)
                                         : UI_ZB_REQ_WRONG_CODE;
        if (c != UI_ZB_REQ_OK) {
            return c;
        }
    }
    const bool allowed = S.port.allowed != NULL && S.port.allowed(S.port.ctx);
    const ui_zb_req_result_t r = zbz_panel_request(&S.panel, S.now, req, allowed);
    if (r == UI_ZB_REQ_OK && !S.unplugged &&
        (req->kind == (uint8_t)UI_ZB_REQ_RESTART || req->kind == (uint8_t)UI_ZB_REQ_RETRY)) {
        /* the service restarts the stack: down, then up again */
        zbf_unplug(&S.radio, true);
        S.restart_until = S.now + RESTART_MS;
    }
    host_zigbee_real_run(S.now);
    return r;
}

void host_zigbee_real_backend(ui_backend_t *out)
{
    out->zb_status = be_zb_status;
    out->zb_devices = be_zb_devices;
    out->zb_device = be_zb_device;
    out->zb_request = be_zb_request;
    out->zb_progress = be_zb_progress;
    out->zb_codes = be_zb_codes;
}

/* ---- the virtual devices, driven from outside ------------------------------------------------- */

static zbf_dev_t *dev_of(host_zbr_dev_t which)
{
    return S.on && (size_t)which < HOST_ZBR_COUNT ? S.dev[which] : NULL;
}

/* After a device's own frame: everything it causes at once (the air's latency included). */
static void settle(void)
{
    host_zigbee_real_run(S.now + ZBF_LATENCY_MS * 4u);
}

bool host_zigbee_real_press(host_zbr_dev_t which)
{
    zbf_dev_t *d = dev_of(which);
    if (d == NULL || S.unplugged) {
        return false;
    }
    const bool joining = !d->joined;
    const bool ok = joining ? zbf_join(&S.radio, d) : zbf_check_in(&S.radio, d);
    const unsigned power = d->power_source & 0x7Fu; /* Basic's power source: 1, 2, 4 mains */
    if (ok && joining && power != 1u && power != 2u && power != 4u) {
        S.battery_at[which] = S.now + BATTERY_MS;
    }
    host_zigbee_real_run(S.now);
    return ok;
}

host_zbr_dev_t host_zigbee_real_press_next(void)
{
    const host_zbr_dev_t n = next_out(HOST_ZBR_COUNT);
    return n != HOST_ZBR_COUNT && window_open() && host_zigbee_real_press(n) ? n : HOST_ZBR_COUNT;
}

bool host_zigbee_real_trip(host_zbr_dev_t which, bool on)
{
    const bool ok = zbf_trip(&S.radio, dev_of(which), on);
    if (ok) {
        settle();
    }
    return ok;
}

bool host_zigbee_real_tamper(host_zbr_dev_t which, bool on)
{
    zbf_dev_t *d = dev_of(which);
    if (d == NULL) {
        return false;
    }
    for (size_t e = 0; e < d->ep_count; e++) {
        if (d->ep[e].zone_type != ZSRC_ZB_ZONE_TYPE_NONE) {
            const uint16_t st = on ? (uint16_t)(d->ep[e].status | ZSRC_ZB_IAS_TAMPER)
                                   : (uint16_t)(d->ep[e].status & (uint16_t)~ZSRC_ZB_IAS_TAMPER);
            const bool ok = zbf_ias_status(&S.radio, d, d->ep[e].sd.endpoint, st);
            if (ok) {
                settle();
            }
            return ok;
        }
    }
    return false;
}

bool host_zigbee_real_battery(host_zbr_dev_t which, uint8_t percent)
{
    const bool ok = zbf_set_battery(&S.radio, dev_of(which), percent);
    if (ok) {
        settle();
    }
    return ok;
}

bool host_zigbee_real_leave(host_zbr_dev_t which)
{
    zbf_dev_t *d = dev_of(which);
    if (d == NULL || !d->joined) {
        return false;
    }
    zbf_leave(&S.radio, d, false);
    settle();
    return true;
}

bool host_zigbee_real_mute(host_zbr_dev_t which, bool on)
{
    zbf_dev_t *d = dev_of(which);
    if (d == NULL) {
        return false;
    }
    /* every step's knob: a step's own (the setup's slow answers) wins over the any-request one */
    const zbf_fault_t mute = {.timeout = true};
    const zbf_fault_t slow = {.delay_ms = SETUP_STEP_MS};
    zbf_fault(d, ZSRC_ZB_STEP_NONE, on ? &mute : NULL);
    for (uint8_t step = (uint8_t)ZSRC_ZB_STEP_NODE_DESC; step < (uint8_t)ZSRC_ZB_STEP_DONE;
         step++) {
        zbf_fault(d, step, on ? &mute : &slow);
    }
    return true;
}

bool host_zigbee_real_stall(host_zbr_dev_t which, uint8_t step)
{
    zbf_dev_t *d = dev_of(which);
    if (d == NULL || step > (uint8_t)ZSRC_ZB_STEP_DONE) {
        return false;
    }
    const zbf_fault_t slow = {.delay_ms = SETUP_STEP_MS};
    for (uint8_t k = (uint8_t)ZSRC_ZB_STEP_NODE_DESC; k < (uint8_t)ZSRC_ZB_STEP_DONE; k++) {
        zbf_fault_t f = slow;
        f.timeout = k == step;
        zbf_fault(d, k, &f);
    }
    return true;
}

/* Its IAS ACE client endpoint (a keypad's, a fob's), or its IAS WD server endpoint (a siren's). */
static zbf_ep_t *ace_or_wd_ep(zbf_dev_t *d)
{
    for (uint8_t e = 0u; d != NULL && e < d->ep_count; e++) {
        const zsrc_zb_simple_desc_t *sd = &d->ep[e].sd;
        for (uint8_t c = 0u; c < sd->out_count; c++) {
            if (sd->out[c] == ZSRC_ZB_ZCL_IAS_ACE) {
                return &d->ep[e];
            }
        }
        for (uint8_t c = 0u; c < sd->in_count; c++) {
            if (sd->in[c] == ZSRC_ZB_ZCL_IAS_WD) {
                return &d->ep[e];
            }
        }
    }
    return NULL;
}

bool host_zigbee_real_arm(host_zbr_dev_t which, uint8_t mode, const char *code)
{
    zbf_dev_t *d = dev_of(which);
    const zbf_ep_t *ep = ace_or_wd_ep(d);
    const size_t n = code != NULL ? strlen(code) : 0u;
    if (d == NULL || !d->joined || ep == NULL || S.unplugged || n + 3u > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return false;
    }
    zsrc_zb_zcl_t c;
    memset(&c, 0, sizeof(c));
    c.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    c.command = ZBA_CMD_ARM;
    c.tsn = ZSRC_ZB_TSN_NEW;
    c.payload[0] = mode;
    c.payload[1] = (uint8_t)n;
    if (n > 0u) {
        memcpy(&c.payload[2], code, n);
    }
    c.payload[2u + n] = 0u; /* zone id */
    c.len = (uint8_t)(3u + n);
    const bool ok = zbf_send(&S.radio, d, ep->sd.endpoint, &c);
    memset(&c, 0, sizeof(c)); /* the code */
    if (ok) {
        settle();
    }
    return ok;
}

uint32_t host_zigbee_real_received(host_zbr_dev_t which, zsrc_zb_zcl_t *last)
{
    const zbf_ep_t *ep = ace_or_wd_ep(dev_of(which));
    if (last != NULL) {
        if (ep != NULL) {
            *last = ep->last_cmd;
        } else {
            memset(last, 0, sizeof(*last));
        }
    }
    return ep != NULL ? ep->commands : 0u;
}

unsigned host_zigbee_real_ace_answered(void)
{
    return S.ace_answered;
}

void host_zigbee_real_unplug(bool unplugged)
{
    if (!S.on || S.unplugged == unplugged) {
        return;
    }
    S.unplugged = unplugged;
    if (S.restart_until == 0u) {
        zbf_unplug(&S.radio, unplugged);
    }
    host_zigbee_real_run(S.now);
}

void host_zigbee_real_noise(bool on)
{
    if (!S.on) {
        return;
    }
    zbf_set_energy(&S.radio, on ? -20 : -85);
    zsrc_zb_energy(&S.src, on ? -20 : -85, S.now); /* as its next energy scan finds it */
    host_zigbee_real_run(S.now);
}

bool host_zigbee_real_joined(host_zbr_dev_t which)
{
    const zbf_dev_t *d = dev_of(which);
    return d != NULL && d->joined;
}

uint64_t host_zigbee_real_ieee(host_zbr_dev_t which)
{
    const zbf_dev_t *d = dev_of(which);
    return d != NULL ? d->ieee : 0u;
}

uint16_t host_zigbee_real_id(host_zbr_dev_t which)
{
    const zbf_dev_t *d = dev_of(which);
    const zbz_device_t *r = d != NULL ? zbz_reg_find(&S.panel.reg, d->ieee) : NULL;
    return r != NULL ? r->id : 0u;
}
