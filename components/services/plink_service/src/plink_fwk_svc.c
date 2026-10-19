/*
 * plink_fwk_svc.c - a keypad's updates from its master on a panel (docs/features/11 "The keypad";
 * LINK-029..036; BL-048): the link core's orders (plink_keypad.h) run through panel_ota on the link
 * task. Keypads follow their master: nobody is asked anything.
 *
 *   - Never past its master (LINK-029): the version its master runs, as INFO names it, is kept in
 *     NVS link/mver with the pairing's net and applied as panel_ota's cap whenever the core's seq
 *     moves (and at the start, before the first INFO). While its session has PLINK_CAP_FW it runs no
 *     scheduled check of its own (panel_ota_set_checks(false)).
 *   - FETCH: panel_ota_order_fetch (a check with want = V at the order's server, the download, the
 *     checks); RELAY: panel_ota_order_relay, then the core's pull feeds panel_ota_write from here
 *     (port.fw_write: the link task's internal-RAM stack writes flash), and panel_ota_end checks the
 *     image once the last byte is in; either way the order's server and key become its trust record
 *     when they differ (panel_ota, LINK-036). The run's state goes back to the core as FW_STATE.
 *   - INSTALL: once the core holds the INSTALL for the READY version (plink_keypad_fw_install_for)
 *     and the gate passes twice about a second apart - the link's part (plink_keypad_fw_hold: a
 *     fresh status, everything disarmed, no panic being sent) and its own (no fire or CO tone kept
 *     through a lost link: plink_keypad_fw_gate KEPT_TONE) - panel_ota_order_install(V), INSTALLING,
 *     and panel_ota restarts it (its 3 s announcement and gate, plink_service_update_idle).
 *   - On trial after an install its master ordered (LINK-034): panel_ota keeps "ordered install of
 *     V" across the restart; the core's trial (plink_keypad_fw_trial) is started at the start and
 *     its state given to panel_ota's self-test (panel_ota_set_trial_check): PASS lets the parts
 *     decide, FAIL goes back.
 * The screens, the console and its getter read a copy published under link_pub_lock.
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "alarm_mem.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "panel_ota.h"

static const char *TAG = "plink_fw";

#define ASK_MS        1000u  /* the gate's second ask */
#define PUT_OFF_MS    15000u /* INSTALLING with no restart this long: panel_ota put it off */
#define PENDING_MS    3000u  /* an order waits this long for the run before it to end */
#define MVER_SCHEMA   1u
#define MVER_MAX      (4u + PLINK_FW_VERSION_MAX)

_Static_assert((int)PANEL_OTA_TRIAL_NONE == (int)PLINK_FWT_NONE &&
                   (int)PANEL_OTA_TRIAL_WAIT == (int)PLINK_FWT_WAIT &&
                   (int)PANEL_OTA_TRIAL_PASS == (int)PLINK_FWT_PASS &&
                   (int)PANEL_OTA_TRIAL_FAIL == (int)PLINK_FWT_FAIL,
               "panel_ota's trial numbers are the link core's");
_Static_assert((int)PLINK_SERVICE_FWK_FETCHING == (int)PLINK_FWS_FETCHING &&
                   (int)PLINK_SERVICE_FWK_RECEIVING == (int)PLINK_FWS_RECEIVING &&
                   (int)PLINK_SERVICE_FWK_VERIFYING == (int)PLINK_FWS_VERIFYING &&
                   (int)PLINK_SERVICE_FWK_READY == (int)PLINK_FWS_READY &&
                   (int)PLINK_SERVICE_FWK_INSTALLING == (int)PLINK_FWS_INSTALLING &&
                   (int)PLINK_SERVICE_FWK_FAILED == (int)PLINK_FWS_FAILED,
               "PLINK_SERVICE_FWK_* are FW_STATE's");

/* The link task's (PSRAM: only it touches them, never with the cache off). */
static struct {
    bool on;
    uint32_t mver_seq;          /* the core's seq last stored and applied */
    bool checks_on;             /* panel_ota_set_checks as last set */
    bool checks_set;
    uint32_t run;               /* the order that started the run panel_ota holds (0: none) */
    uint8_t run_op;
    char v[PLINK_FW_VERSION_MAX + 1];
    uint32_t size;
    bool relay_open;            /* the relay's writer is open (panel_ota_order_relay) */
    bool all_in;                /* its last byte is in: panel_ota_end is due */
    bool write_failed;          /* panel_ota_write refused (the run ends) */
    uint8_t reported;           /* FW_STATE's state as last reported */
    uint8_t asks;               /* the gate passed once ... */
    uint64_t ask_at;            /* ... and is asked again then */
    uint64_t installing_at;     /* INSTALLING reported then (0: not) */
    bool pending;               /* an order waits for the run before it to end */
    plink_fw_order_t order;
    uint64_t pending_until;
    uint32_t probe_seq;
    uint8_t last_probation, last;
    char last_version[PLINK_FW_VERSION_MAX + 1];
    uint32_t ota_seq;
    panel_ota_status_t st;
    panel_ota_order_status_t ost;
} KF ALARM_SLOW_BSS;

static volatile uint8_t s_trial; /* plink_fw_trial_t, for panel_ota's self-test (internal RAM) */

/* What the screens read (link_pub_lock). */
static plink_service_fw_keypad_t s_pubk ALARM_SLOW_BSS;
static bool s_pubk_on;

static void put(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src != NULL ? src : "");
}

#define PUT(dst, src) put((dst), sizeof(dst), (src))

uint16_t kfw_caps(void)
{
    return (uint16_t)PLINK_CAP_FW;
}

/* panel_ota's self-test asks the trial (the update task, once a second). */
static uint8_t trial_check(void *ctx)
{
    (void)ctx;
    return s_trial;
}

/* ---- never past its master (LINK-029) --------------------------------------------------------- */

static void cap_apply(plink_keypad_t *k, uint64_t now)
{
    const char *v = "";
    const bool capped = plink_keypad_version_cap(k, &v, NULL);
    plink_keypad_info_t info;
    plink_keypad_info(k, now, &info);
    proto_version_t pv;
    const proto_fw_cap_t cap = !capped                                     ? PROTO_FW_CAP_NONE
                               : v[0] != '\0' && proto_version_parse(v, &pv) ? PROTO_FW_CAP_AT
                                                                            : PROTO_FW_CAP_UNKNOWN;
    panel_ota_set_version_cap(cap, v, info.master_name);
}

/* link/mver: schema, net, version; erased when there is none (the link task: internal stack). */
static void mver_store(const char *version, uint16_t net)
{
    if (version[0] == '\0') {
        (void)link_nvs_write(LINK_KEY_MVER, NULL, 0u);
        return;
    }
    uint8_t buf[MVER_MAX];
    const size_t n = strnlen(version, PLINK_FW_VERSION_MAX);
    buf[0] = MVER_SCHEMA;
    buf[1] = (uint8_t)(net & 0xFFu);
    buf[2] = (uint8_t)(net >> 8);
    buf[3] = (uint8_t)n;
    memcpy(buf + 4, version, n);
    const esp_err_t err = link_nvs_write(LINK_KEY_MVER, buf, 4u + n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "link/mver: %s", esp_err_to_name(err));
    }
}

static bool mver_load(char *version, size_t cap, uint16_t *net)
{
    uint8_t buf[MVER_MAX];
    size_t len = 0;
    if (!link_nvs_read(LINK_KEY_MVER, buf, sizeof buf, &len) || len < 4u ||
        buf[0] != MVER_SCHEMA || buf[3] > PLINK_FW_VERSION_MAX || len < 4u + buf[3] ||
        (size_t)buf[3] + 1u > cap) {
        return false; /* none, or another firmware's layout: never misread */
    }
    *net = (uint16_t)((unsigned)buf[1] | ((unsigned)buf[2] << 8));
    memcpy(version, buf + 4, buf[3]);
    version[buf[3]] = '\0';
    return true;
}

/* ---- the start ------------------------------------------------------------------------------- */

static void last_report(plink_keypad_t *k, uint64_t now)
{
    const uint8_t probation = KF.st.probation ? 1u : 0u;
    const uint8_t last = (uint8_t)KF.st.last.result;
    if (probation == KF.last_probation && last == KF.last &&
        strcmp(KF.last_version, KF.st.last.version) == 0) {
        return;
    }
    KF.last_probation = probation;
    KF.last = last;
    PUT(KF.last_version, KF.st.last.version);
    plink_keypad_fw_last(k, now, probation, last, KF.st.last.version,
                         (uint8_t)(KF.st.last.failed & 0xFFu));
}

void kfw_boot(plink_keypad_t *k, uint64_t now)
{
    memset(&KF, 0, sizeof KF);
    KF.last = 0xFFu;
    /* its master's version back from link/mver, so the cap holds before the first INFO */
    char mv[PLINK_FW_VERSION_MAX + 1] = "";
    uint16_t net = 0u;
    const bool stored = mver_load(mv, sizeof mv, &net);
    if (stored) {
        plink_keypad_master_version_restore(k, mv, net);
    }
    (void)plink_keypad_master_version(k, &KF.mver_seq);
    if (stored && plink_keypad_master_version(k, NULL)[0] == '\0') {
        mver_store("", 0u); /* another pairing's (a power cut before it was erased) */
    }
    cap_apply(k, now); /* right after the restore, whatever the seq says */
    /* its facts and its last result */
    panel_ota_facts_t f;
    panel_ota_get_facts(&f);
    plink_fw_info_t info;
    memset(&info, 0, sizeof info);
    info.can = PLINK_FWI_FETCH | PLINK_FWI_RELAY | PLINK_FWI_KEY_ACCEPT; /* takes its master's key */
    PUT(info.device, f.device);
    PUT(info.board_rev, f.board_rev);
    PUT(info.chip_rev, f.chip_rev);
    info.chip_rev_full = f.chip_rev_full;
    PUT(info.project, f.project);
    PUT(info.version, f.version);
    info.slot = f.slot;
    if (f.has_own_key) {
        memcpy(info.own_key8, f.own_key, sizeof info.own_key8);
    }
    if (f.has_accepted_key) {
        memcpy(info.accepted_key8, f.accepted_key, sizeof info.accepted_key8);
    }
    PUT(info.cp_version, f.cp_version);
    plink_keypad_fw_info(k, now, &info);
    panel_ota_get_status(&KF.st);
    KF.ota_seq = panel_ota_seq();
    last_report(k, now);
    /* on trial with its master after an install it ordered (LINK-034) */
    char v[32] = "";
    if (panel_ota_ordered_trial(v) && strcmp(v, f.version) == 0) {
        plink_keypad_fw_trial(k, now, v);
        ESP_LOGW(TAG, "on trial with its master: %s stays only once its master runs it", v);
    }
    s_trial = plink_keypad_fw_trial_state(k, now);
    panel_ota_set_trial_check(trial_check, NULL);
    KF.on = true;
}

/* ---- the run --------------------------------------------------------------------------------- */

static void report(plink_keypad_t *k, uint64_t now, uint8_t state, uint8_t why)
{
    plink_keypad_fw_report(k, now, KF.run, state, why);
    KF.reported = state;
}

static void run_end(void)
{
    KF.run = 0u;
    KF.run_op = 0u;
    KF.v[0] = '\0';
    KF.relay_open = false;
    KF.all_in = false;
    KF.write_failed = false;
    KF.asks = 0u;
    KF.installing_at = 0u;
}

/* A refusal or a failure as panel_ota names it (its why), else what the error says. */
static uint8_t why_of(esp_err_t err)
{
    panel_ota_order_get(&KF.ost);
    if (KF.ost.why != 0u) {
        return KF.ost.why;
    }
    switch (err) {
    case ESP_ERR_INVALID_ARG: return PLINK_FW_WHY_BAD_ORDER;
    case ESP_ERR_INVALID_VERSION: return PLINK_FW_WHY_NOT_NEWER;
    case ESP_ERR_INVALID_SIZE: return PLINK_FW_WHY_TOO_BIG;
    case ESP_ERR_NOT_ALLOWED: return PLINK_FW_WHY_WRONG_VERSION;
    default: return PLINK_FW_WHY_BUSY;
    }
}

/* FETCH or a new RELAY through panel_ota (whatever run it held has ended). */
static void begin(plink_keypad_t *k, uint64_t now, const plink_fw_order_t *o)
{
    plink_keypad_info_t info;
    plink_keypad_info(k, now, &info);
    const panel_ota_order_t po = {
        .version = o->version,
        .server = o->server,
        .channel = o->channel,
        .master = info.master_name,
        .key = o->key,
    };
    run_end();
    KF.run = o->order;
    KF.run_op = o->op;
    PUT(KF.v, o->version);
    esp_err_t err;
    if (o->op == PLINK_FW_OP_FETCH) {
        err = panel_ota_order_fetch(&po);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "ordered: fetch %s", o->version);
            report(k, now, PLINK_FWS_FETCHING, PLINK_FW_WHY_OK);
            return;
        }
    } else {
        KF.size = o->size;
        err = panel_ota_order_relay(&po, o->size, o->sha256); /* the writer, on this task */
        if (err == ESP_OK) {
            KF.relay_open = true;
            ESP_LOGI(TAG, "ordered: relay %s, %" PRIu32 " bytes", o->version, o->size);
            report(k, now, PLINK_FWS_RECEIVING, PLINK_FW_WHY_OK); /* the pull starts */
            return;
        }
    }
    const uint8_t why = why_of(err);
    ESP_LOGW(TAG, "ordered %s of %s refused: %s (why %u)",
             o->op == PLINK_FW_OP_FETCH ? "fetch" : "relay", o->version, esp_err_to_name(err),
             (unsigned)why);
    report(k, now, PLINK_FWS_FAILED, why);
    run_end();
}

size_t kfw_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len, uint32_t *busy_ms)
{
    (void)ctx;
    if (!KF.relay_open || KF.write_failed || KF.all_in) {
        return 0u;
    }
    const int64_t t0 = esp_timer_get_time();
    char why[64];
    const esp_err_t err = panel_ota_write(data, len, why, sizeof why);
    const int64_t took = esp_timer_get_time() - t0;
    *busy_ms = took > 0 ? (uint32_t)(took / 1000) : 0u; /* the flash was busy about that long */
    if (err != ESP_OK) {
        KF.write_failed = true; /* the run ends at the next poll, with panel_ota's why */
        ESP_LOGW(TAG, "relay write at %" PRIu32 ": %s", offset, why);
        return 0u;
    }
    if ((uint64_t)offset + len >= KF.size) {
        KF.all_in = true;
    }
    return len;
}

/* The order the core hands over. */
static void take(plink_keypad_t *k, uint64_t now, const plink_fw_order_t *o)
{
    switch (o->op) {
    case PLINK_FW_OP_FETCH:
    case PLINK_FW_OP_RELAY:
        panel_ota_order_get(&KF.ost);
        if (KF.ost.active) {
            (void)panel_ota_order_cancel(); /* over within a second: the order waits for it */
            KF.pending = true;
            KF.order = *o;
            KF.pending_until = now + PENDING_MS;
            run_end();
            return;
        }
        begin(k, now, o);
        break;
    case PLINK_FW_OP_CANCEL:
        KF.pending = false;
        if (KF.relay_open && o->order == 0u) {
            plink_fw_state_t st;
            plink_keypad_fw_status(k, now, &st); /* the core's why: stalled, timed out */
            (void)panel_ota_order_fail(st.why != 0u ? st.why : (uint8_t)PLINK_FW_WHY_STALLED);
        } else if (KF.run != 0u || KF.ost.active) {
            (void)panel_ota_order_cancel();
        }
        run_end();
        break;
    case PLINK_FW_OP_PROBE: {
        /* not sent by the simple planner; answered all the same */
        plink_keypad_info_t info;
        plink_keypad_info(k, now, &info);
        const panel_ota_order_t po = {.version = o->version,
                                      .server = o->server,
                                      .channel = o->channel,
                                      .master = info.master_name,
                                      .key = NULL};
        if (panel_ota_order_probe(&po) != ESP_OK) {
            plink_keypad_fw_probe(k, now, PLINK_FW_WHY_BAD_ORDER, "", NULL, false);
        }
        break;
    }
    default: break; /* INFO: the same facts; INSTALL: the gate, below */
    }
}

/* panel_ota's run as FW_STATE says it. */
static void follow(plink_keypad_t *k, uint64_t now)
{
    const panel_ota_order_status_t *ost = &KF.ost;
    if (ost->probed && ost->seq != KF.probe_seq) {
        KF.probe_seq = ost->seq;
        plink_keypad_fw_probe(k, now, ost->probe_why, ost->offered,
                              ost->offered_has_key ? ost->offered_key : NULL, ost->key_trusted);
    }
    if (KF.run == 0u) {
        return;
    }
    if (KF.write_failed) {
        report(k, now, PLINK_FWS_FAILED, why_of(ESP_FAIL));
        run_end();
        return;
    }
    if (KF.all_in && KF.relay_open) {
        report(k, now, PLINK_FWS_VERIFYING, PLINK_FW_WHY_OK);
        char version[32];
        char why[64];
        KF.relay_open = false;
        const esp_err_t err = panel_ota_end(version, sizeof version, why, sizeof why);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "the relayed image of %s: %s", KF.v, why);
            report(k, now, PLINK_FWS_FAILED, why_of(err));
            run_end();
            return;
        }
        panel_ota_order_get(&KF.ost); /* READY: waits for its master's INSTALL */
    }
    if (!ost->active || strcmp(ost->version, KF.v) != 0) {
        /* panel_ota ended it (a failure, a stop): the core's run ends with its why */
        if (KF.installing_at == 0u) {
            report(k, now, PLINK_FWS_FAILED,
                   ost->why != 0u ? ost->why : (uint8_t)PLINK_FW_WHY_CANCELLED);
            run_end();
        }
        return;
    }
    if (ost->ready) {
        if (KF.installing_at == 0u && KF.reported != PLINK_FWS_READY) {
            report(k, now, PLINK_FWS_READY, PLINK_FW_WHY_OK);
        }
        return;
    }
    if (KF.run_op == PLINK_FW_OP_FETCH && KF.st.kind == PANEL_OTA_KIND_RELEASE) {
        if (KF.st.stage == PANEL_OTA_STAGE_VERIFYING && KF.reported != PLINK_FWS_VERIFYING) {
            report(k, now, PLINK_FWS_VERIFYING, PLINK_FW_WHY_OK);
        } else if (KF.st.stage == PANEL_OTA_STAGE_DOWNLOADING && KF.st.size != 0u) {
            plink_keypad_fw_progress(k, now, KF.run, KF.st.bytes, KF.st.size, NULL);
        }
    }
}

/* The gate, asked twice about a second apart, once the core holds the INSTALL for the READY image;
 * then the restart is panel_ota's (its announcement, its gate). Put off there (OTA-028): READY
 * again, the next INSTALL's gate asked anew. */
static void gate(plink_keypad_t *k, uint64_t now, bool kept_tone)
{
    plink_keypad_fw_gate(k, now, kept_tone ? (uint8_t)PLINK_FW_WHY_KEPT_TONE : 0u);
    const panel_ota_order_status_t *ost = &KF.ost;
    if (KF.installing_at != 0u) {
        if (KF.st.phase != PANEL_OTA_RESTARTING && now - KF.installing_at >= PUT_OFF_MS) {
            KF.installing_at = 0u; /* put off: READY again */
            KF.asks = 0u;
            report(k, now, PLINK_FWS_READY, PLINK_FW_WHY_OK);
        }
        return;
    }
    if (KF.run == 0u || !ost->active || !ost->ready || !plink_keypad_fw_install_for(k, ost->version)) {
        KF.asks = 0u;
        return;
    }
    if (plink_keypad_fw_hold(k, now) != 0u || kept_tone) {
        KF.asks = 0u;
    } else if (KF.asks == 0u) {
        KF.asks = 1u;
        KF.ask_at = now + ASK_MS;
    } else if (now >= KF.ask_at) {
        if (panel_ota_order_install(ost->version) == ESP_OK) {
            KF.installing_at = now;
            report(k, now, PLINK_FWS_INSTALLING, PLINK_FW_WHY_OK);
            ESP_LOGW(TAG, "installing %s: restarting with its master", ost->version);
        }
        KF.asks = 0u;
    }
}

static void publish(plink_keypad_t *k, uint64_t now, const plink_keypad_info_t *info)
{
    plink_fw_state_t st;
    plink_keypad_fw_status(k, now, &st);
    plink_service_fw_keypad_t p;
    memset(&p, 0, sizeof p);
    p.follows = info->state != PLINK_LINK_NOT_PAIRED && (info->caps & PLINK_CAP_FW) != 0u;
    PUT(p.master, info->master_name);
    PUT(p.master_version, plink_keypad_master_version(k, NULL));
    PUT(p.version, KF.st.running);
    switch (st.state) {
    case PLINK_FWS_FETCHING:
    case PLINK_FWS_RECEIVING:
    case PLINK_FWS_VERIFYING:
    case PLINK_FWS_READY:
    case PLINK_FWS_INSTALLING:
    case PLINK_FWS_FAILED: p.state = st.state; break;
    default: p.state = PLINK_SERVICE_FWK_IDLE; break;
    }
    if (p.state != PLINK_SERVICE_FWK_IDLE) {
        PUT(p.target, st.target);
    }
    if ((p.state == PLINK_SERVICE_FWK_FETCHING || p.state == PLINK_SERVICE_FWK_RECEIVING) &&
        st.size != 0u) {
        const uint64_t pc = (uint64_t)st.bytes * 100u / st.size;
        p.percent = (uint8_t)(pc > 100u ? 100u : pc);
    }
    p.paused = (st.flags & PLINK_FWF_PAUSED) != 0u;
    p.banner = p.state >= PLINK_SERVICE_FWK_FETCHING && p.state <= PLINK_SERVICE_FWK_INSTALLING;
    p.on_trial = plink_keypad_fw_trial_state(k, now) == PLINK_FWT_WAIT;
    link_pub_lock();
    s_pubk = p;
    s_pubk_on = true;
    link_pub_unlock();
}

void kfw_poll(plink_keypad_t *k, uint64_t now, const plink_keypad_info_t *info, bool kept_tone)
{
    if (!KF.on) {
        return;
    }
    /* its master's version kept and the cap applied whenever the core's seq moves */
    uint32_t seq = 0u;
    const char *mv = plink_keypad_master_version(k, &seq);
    if (seq != KF.mver_seq) {
        KF.mver_seq = seq;
        uint16_t net = 0u;
        (void)plink_keypad_version_cap(k, NULL, &net);
        mver_store(mv, mv[0] != '\0' ? net : 0u);
        cap_apply(k, now);
    }
    /* no scheduled check of its own while its master orders its updates */
    const bool checks = !(info->state != PLINK_LINK_NOT_PAIRED && (info->caps & PLINK_CAP_FW) != 0u);
    if (!KF.checks_set || checks != KF.checks_on) {
        KF.checks_set = true;
        KF.checks_on = checks;
        panel_ota_set_checks(checks);
    }
    const uint32_t ota = panel_ota_seq();
    if (ota != KF.ota_seq) {
        KF.ota_seq = ota;
        panel_ota_get_status(&KF.st);
        last_report(k, now);
    }
    panel_ota_order_get(&KF.ost);
    plink_fw_order_t o;
    while (plink_keypad_take_fw_order(k, &o)) {
        take(k, now, &o);
    }
    if (KF.pending) {
        panel_ota_order_get(&KF.ost);
        if (!KF.ost.active) {
            KF.pending = false;
            begin(k, now, &KF.order);
        } else if (now >= KF.pending_until) {
            KF.pending = false;
            KF.run = KF.order.order;
            report(k, now, PLINK_FWS_FAILED, PLINK_FW_WHY_BUSY);
            run_end();
        }
    }
    panel_ota_order_get(&KF.ost); /* as the orders just left it */
    follow(k, now);
    gate(k, now, kept_tone);
    s_trial = plink_keypad_fw_trial_state(k, now);
    publish(k, now, info);
}

bool plink_service_fw_keypad(plink_service_fw_keypad_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (plink_service_role() != PLINK_SERVICE_KEYPAD) {
        return false;
    }
    link_pub_lock();
    const bool on = s_pubk_on;
    if (on) {
        *out = s_pubk;
    }
    link_pub_unlock();
    return on;
}

int kfw_console_show(void)
{
    plink_service_fw_keypad_t p;
    if (!plink_service_fw_keypad(&p)) {
        printf("keypad updates: off (the link isn't running)\n");
        return 1;
    }
    static const char *const RUN[] = {"idle",      "probing", "fetching",   "receiving", "key wait",
                                      "verifying", "ready",   "installing", "failed",    "stopped"};
    char cap[32] = "";
    const proto_fw_cap_t c = panel_ota_get_version_cap(cap, NULL);
    printf("This keypad runs %s; %s\n", p.version,
           p.follows ? "its updates come from its master" : "not following a master");
    printf("  its master \"%s\" runs %s; cap: %s %s\n", p.master,
           p.master_version[0] != '\0' ? p.master_version : "(not heard)",
           c == PROTO_FW_CAP_NONE ? "none" : c == PROTO_FW_CAP_AT ? "at" : "unknown (nothing)", cap);
    printf("  run: %s%s%s", p.state < 10u ? RUN[p.state] : "?", p.target[0] != '\0' ? " " : "",
           p.target);
    if (p.state == PLINK_SERVICE_FWK_FETCHING || p.state == PLINK_SERVICE_FWK_RECEIVING) {
        printf(" %u %%%s", (unsigned)p.percent, p.paused ? " (paused)" : "");
    }
    printf("%s%s\n", p.banner ? "; Home shows the update banner" : "",
           p.on_trial ? "; on trial with its master" : "");
    return 0;
}

#endif /* CONFIG_ALARM_LINK */
