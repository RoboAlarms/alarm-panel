/*
 * plink_keypad_svc.c - a keypad's side of the panel link on a panel (docs/features/11, BL-048):
 * the keypad's link manager (plink_keypad.h) on the link task, and what a panel in the keypad role
 * does with it:
 *   - finding its master: mDNS `_roboalarms-link._udp` (TXT net = its master's net id; while
 *     pairing, the ones with pair=1), the subnet broadcast when mDNS finds nothing, or an address
 *     set by hand (`plink master <ip>`, stored) (LINK-021);
 *   - the copies the UI reads (snapshot, configuration, events, the link's state) and the UI's
 *     commands, answered within 3 s or refused at once while the link isn't up - panic excepted
 *     (LINK-023);
 *   - the sounds: one-shots from the master, the home partition's continuous pattern, fire and CO
 *     from any partition; on a lost link only fire and CO keep sounding, until OFF is pressed here
 *     or the link is back (decided 2026-10-15): OFF is a DISARM or SILENCE the master didn't run
 *     (no link, or no answer), and link_info.local_alarm asks Home for its Silence meanwhile;
 *   - the time: the master's clock and zone, and no time server of its own while paired;
 *   - the update gate and the link's heartbeat for the update's self-test (LINK-020).
 * A master whose setup makes it a keypad uses the same manager for that pairing only (Option C):
 * the record it makes is kept in RAM until the setup's Done stores it with the keypad role, and
 * nothing is sent to the other master after the pairing until then (no handshake: it keeps the new
 * keypad provisional, and drops it by itself after 10 minutes if the setup is abandoned, LINK-016).
 * Leaving the keypad branch without Done (KP_PAIR_STOP) forgets the pairing and ends the manager
 * (ksvc_setup_end).
 */
#include "plink_internal.h"

#if CONFIG_ALARM_LINK

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "alarm/alarm_codec.h"
#include "alarm_mem.h"
#include "alarm_proto/proto_fw.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "i18n/i18n.h"
#include "mdns.h"
#include "net_wifi.h"
#include "panel_link/plink_view.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "sounder.h"

static const char *TAG = "plink";

#define DISCOVERY_EVERY_MS 10000u /* while paired and not online: look for the master again */
#define MDNS_QUERY_MS      1500u
#define MDNS_RESULTS       4u
#define PAIR_LOOK_EVERY_MS 3000u
#define PAIR_LOOK_MS       10000u /* mDNS alone, then the subnet broadcast too */
/* After PAIR_LOOK_MS without an mDNS answer the keypad asks by the subnet broadcast: the core's
 * pairing takes the first PAIR_COMMIT's (unicast) address as its master's (LINK-021). */
#define PAIR_BROADCAST     1
#define PAIR_GIVE_UP_MS    30000u /* no alarm panel answered: UI_LINK_FAIL_NOT_FOUND (E-18) */
#define CLOCK_CHECK_MS     10000u
#define CLOCK_STEP_MS      2000
#define PUBLISH_MS         500u
#define NET_CHECK_MS       1000u
#define BEAT_MS            1000u
#define CMD_WAIT_MS        3000u /* the UI's wait for a command's answer (docs/features/11) */
#define MAINT_CHECK_MS     2000u
/* An OFF the master didn't run still counts for a fire or CO tone kept through a lost link when the
 * link goes LOST this soon after it (an OFF keyed while ONLINE but the master already silent). */
#define OFF_KEEP_MS        PLINK_LOST_MS

typedef enum { PAIR_NONE = 0, PAIR_LOOKING, PAIR_ASKED, PAIR_FAILED } pair_phase_t;
typedef enum { FIND_NONE = 0, FIND_MASTER, FIND_PAIRING } find_t;

typedef struct {
    uint32_t ip; /* network order */
    uint16_t port;
    uint16_t net;
    bool pair;
    char id[PLINK_PANEL_ID_LEN + 1];
} found_t;

static struct {
    plink_keypad_t *k;   /* PSRAM; NULL until prepared */
    uint8_t *cfg_buf;    /* the configuration being pulled, PSRAM */
    alarm_config_t *cfg; /* the decoded one, PSRAM */
    bool setup_only;     /* a master's setup pairing (Option C): nothing else runs */
    bool paired;         /* a record is stored: */
    uint16_t net_id;     /* its master's net */
    char master_id[PLINK_PANEL_ID_LEN + 1];
    uint8_t home;
    bool pending; /* Option C: the record the pairing made, until the setup's Done */
    plink_keypad_rec_t pending_rec;
    uint32_t static_ip; /* `plink master <ip>` (network order), 0 none */
    /* mDNS */
    mdns_search_once_t *search;
    uint8_t search_for; /* find_t */
    uint64_t next_search_at;
    bool resolve; /* the manager asked to look for the master again (port.resolve) */
    uint8_t misses;
    bool gave_addr;
    plink_addr_t given;
    bool given_bcast; /* given is the subnet broadcast */
    /* pairing a person asked for */
    uint8_t pair_phase; /* pair_phase_t */
    uint8_t pair_fail;  /* UI_LINK_FAIL_* while PAIR_FAILED */
    uint64_t pair_started;
    /* the UI's command in flight */
    bool cmd_active;
    uint32_t cmd_gen;
    uint32_t cmd_req;
    /* sounds */
    uint8_t cont;   /* the continuous pattern sounding (alarm_sound_t) */
    bool local_off; /* OFF pressed here while the link is down: fire or CO stopped */
    /* the rest */
    uint64_t clock_at, net_at, publish_at, beat_at, events_at;
    char ip[16]; /* the last address it had: kept across a drop, so a new one is told at once */
    bool net_up;
    bool sntp_off;
    uint8_t last_home;
    uint32_t home_gen;
    bool copied;
    uint32_t seq_copied, cfg_seq_copied;
    uint32_t bad_crc;     /* the configuration it couldn't read, as last seen */
    uint32_t bad_cfg_seq; /* ... and the configuration count then: a later install outdates it */
} K;

/* The manager and its buffers (PSRAM): allocated at the first prepare and kept, so the setup's
 * pairing can end (ksvc_setup_end) and start again without allocating again. */
static plink_keypad_t *s_kmem;
static uint8_t *s_cfg_buf_mem;
static alarm_config_t *s_cfg_mem;

/* The UI's copies (link_pub_lock), PSRAM; the keypad role only. */
static alarm_snapshot_t *s_ui_snap;
static alarm_config_t *s_ui_cfg;
static alarm_event_t *s_ui_ev;
static alarm_event_t *s_ev_tmp; /* the link task's */
static uint32_t s_ui_seq;
static bool s_ui_have_cfg;
static size_t s_ui_ev_n;

/* The UI's commands (ksvc_command): one at a time, the codes wiped once handed over. */
static SemaphoreHandle_t s_cmd_mutex;
static SemaphoreHandle_t s_cmd_done;
static portMUX_TYPE s_cmd_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool pending;
    uint32_t gen;
    plink_cmd_t cmd;
} s_box;
static struct {
    uint32_t gen;
    alarm_result_t r;
    plink_result_t res;
} s_ans;
static uint32_t s_cmd_gen;
static atomic_uint s_cmd_abandon; /* the gen the UI stopped waiting for */
/* OFF (disarm, silence) keyed here that the master didn't run, and when (s_cmd_mux). */
static struct {
    bool set;
    uint64_t at;
} s_off;
/* An update let through by LINK-025's maintenance (link/otamaint): marked before it restarts the
 * panel; after that restart, while marked, the link beats for the self-test without its master
 * (the link task runs: socket bound, crypto proved). Cleared once the image is confirmed or the
 * link is up again. */
static atomic_bool s_maint_marked;
static atomic_bool s_maint_beat;
static uint64_t s_maint_at;
static atomic_uint s_power_flags; /* PLINK_HB_* from the health task */

bool ksvc_active(void)
{
    return K.k != NULL;
}

/* ---- the manager's port ---------------------------------------------------------------------- */

static bool port_send(void *ctx, const plink_addr_t *to, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (K.setup_only && K.pending) {
        /* Option C, paired: no handshake before the setup's Done, so the other master keeps the
         * new keypad provisional and drops it by itself if the setup is abandoned (LINK-016). */
        return false;
    }
    return link_send(to, frame, len);
}

static void remember_master(const plink_keypad_rec_t *rec)
{
    K.paired = rec != NULL;
    K.net_id = rec != NULL ? rec->net_id : 0u;
    snprintf(K.master_id, sizeof K.master_id, "%s", rec != NULL ? rec->panel_id : "");
    K.home = rec != NULL ? rec->home : 1u;
    K.misses = 0;
    K.gave_addr = false;
}

static bool port_store(void *ctx, const plink_keypad_rec_t *rec)
{
    (void)ctx;
    if (K.setup_only) {
        /* Option C: stored with the keypad role by the setup's Done (ksvc_setup_done) */
        if (rec != NULL) {
            K.pending_rec = *rec;
            K.pending = true;
        } else {
            plink_wipe(&K.pending_rec, sizeof K.pending_rec);
            K.pending = false;
        }
        return true;
    }
    if (rec == NULL) {
        const bool ok = link_nvs_write(LINK_KEY_KEYPAD, NULL, 0) == ESP_OK;
        if (ok) {
            remember_master(NULL);
        }
        return ok;
    }
    uint8_t buf[PLINK_STORE_MAX]; /* on the internal stack: NVS reads it with the cache off */
    size_t len = 0;
    const bool ok = plink_store_keypad_encode(rec, buf, sizeof buf, &len) &&
                    link_nvs_write(LINK_KEY_KEYPAD, buf, len) == ESP_OK;
    plink_wipe(buf, sizeof buf);
    if (ok) {
        remember_master(rec);
    } else {
        ESP_LOGE(TAG, "the master's record could not be stored");
    }
    return ok;
}

/* The manager's handshake went unanswered: look for the master again before its next try
 * (docs/features/11 "Sessions"). Called inside the manager: only a flag here. */
static void port_resolve(void *ctx)
{
    (void)ctx;
    K.resolve = true;
}

/* ---- finding the master: mDNS, the broadcast, an address set by hand (LINK-021) -------------- */

static void search_stop(void)
{
    if (K.search != NULL) {
        mdns_query_async_delete(K.search);
        K.search = NULL;
    }
    K.search_for = FIND_NONE;
}

static void search_start(uint8_t what)
{
    if (K.search != NULL || !link_mdns_init()) {
        return;
    }
    K.search = mdns_query_async_new(NULL, LINK_MDNS_SERVICE, LINK_MDNS_PROTO, MDNS_TYPE_PTR,
                                    MDNS_QUERY_MS, MDNS_RESULTS, NULL);
    K.search_for = K.search != NULL ? what : FIND_NONE;
}

/* The finished search's alarm panels (one each); *done false while it still runs. */
static size_t search_take(found_t *out, size_t max, bool *done)
{
    *done = false;
    if (K.search == NULL) {
        return 0u;
    }
    mdns_result_t *results = NULL;
    uint8_t count = 0;
    if (!mdns_query_async_get_results(K.search, 0, &results, &count)) {
        return 0u;
    }
    *done = true;
    size_t n = 0;
    for (const mdns_result_t *r = results; r != NULL && n < max; r = r->next) {
        found_t f;
        memset(&f, 0, sizeof f);
        for (const mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4 && a->addr.u_addr.ip4.addr != 0u) {
                f.ip = a->addr.u_addr.ip4.addr;
                break;
            }
        }
        f.port = r->port != 0u ? r->port : (uint16_t)PLINK_UDP_PORT;
        for (size_t i = 0; f.ip != 0u && i < r->txt_count; i++) {
            const char *key = r->txt[i].key;
            const char *val = r->txt[i].value;
            if (key == NULL || val == NULL) {
                continue;
            }
            const size_t vl = r->txt_value_len != NULL ? r->txt_value_len[i] : strlen(val);
            char v[16];
            snprintf(v, sizeof v, "%.*s", (int)(vl < sizeof v ? vl : sizeof v - 1u), val);
            if (strcmp(key, "net") == 0) {
                f.net = (uint16_t)strtoul(v, NULL, 16);
            } else if (strcmp(key, "pair") == 0) {
                f.pair = strcmp(v, "1") == 0;
            } else if (strcmp(key, "id") == 0) {
                snprintf(f.id, sizeof f.id, "%.*s", (int)PLINK_PANEL_ID_LEN, v);
            }
        }
        bool seen = f.ip == 0u;
        for (size_t j = 0; j < n && !seen; j++) {
            seen = out[j].ip == f.ip || (f.id[0] != '\0' && strcmp(out[j].id, f.id) == 0);
        }
        if (!seen) {
            out[n++] = f;
        }
    }
    if (results != NULL) {
        mdns_query_results_free(results);
    }
    mdns_query_async_delete(K.search);
    K.search = NULL;
    return n;
}

static plink_addr_t addr_of(const found_t *f)
{
    uint8_t ip[4];
    memcpy(ip, &f->ip, sizeof ip);
    return plink_addr_udp(ip, f->port);
}

/* broadcast: addr is this panel's subnet broadcast (link_subnet_broadcast), which the core can't
 * tell from a unicast address by its bytes (LINK-021). */
static void give_master(uint64_t now, const plink_addr_t *addr, bool broadcast)
{
    if (!K.gave_addr || !plink_addr_equal(&K.given, addr) || K.given_bcast != broadcast) {
        char text[24];
        link_addr_ip(addr, text);
        ESP_LOGI(TAG, "looking for the alarm panel at %s%s", text, broadcast ? " (broadcast)" : "");
        /* it moves only on a sealed frame from there */
        plink_keypad_master_addr(K.k, now, addr, broadcast);
        K.given = *addr;
        K.given_bcast = broadcast;
        K.gave_addr = true;
    }
}

static void pair_fail(uint8_t why)
{
    search_stop();
    K.pair_phase = PAIR_FAILED;
    K.pair_fail = why;
}

static void pair_begin(uint64_t now, uint32_t ip)
{
    search_stop();
    K.pair_started = now;
    K.pair_fail = UI_LINK_FAIL_NONE;
    if (ip != 0u) {
        const plink_addr_t addr = link_addr_of(ip);
        plink_keypad_pair_start(K.k, now, &addr, false, 0u);
        K.pair_phase = PAIR_ASKED;
    } else {
        K.pair_phase = PAIR_LOOKING;
        K.next_search_at = now;
    }
}

static void pair_end(void)
{
    if (K.search_for == FIND_PAIRING) {
        search_stop();
    }
    K.pair_phase = PAIR_NONE;
}

static void found_for_pairing(uint64_t now, const found_t *f, size_t n)
{
    const found_t *open = NULL;
    size_t opened = 0;
    for (size_t i = 0; i < n; i++) {
        if (f[i].pair) {
            open = &f[i];
            opened++;
        }
    }
    if (opened > 1u) {
        pair_fail(UI_LINK_FAIL_TWO_MASTERS); /* "More than one alarm panel is open for pairing" */
    } else if (open != NULL) {
        const plink_addr_t addr = addr_of(open);
        plink_keypad_pair_start(K.k, now, &addr, false, open->net);
        K.pair_phase = PAIR_ASKED;
    }
}

static void found_master(uint64_t now, const found_t *f, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const bool same_panel =
            f[i].id[0] == '\0' || K.master_id[0] == '\0' || strcmp(f[i].id, K.master_id) == 0;
        if (f[i].net == K.net_id && same_panel) {
            K.misses = 0;
            const plink_addr_t addr = addr_of(&f[i]);
            give_master(now, &addr, false);
            return;
        }
    }
    if (++K.misses >= 2u) {
        plink_addr_t b; /* mDNS blocked or lost: the subnet broadcast (LINK-021) */
        link_subnet_broadcast(&b);
        give_master(now, &b, true);
    }
}

static void discovery(uint64_t now, const plink_keypad_info_t *info)
{
    found_t f[MDNS_RESULTS];
    bool done = false;
    const uint8_t what = K.search_for;
    const size_t n = search_take(f, MDNS_RESULTS, &done);
    if (done) {
        K.search_for = FIND_NONE;
        if (what == FIND_PAIRING && K.pair_phase == PAIR_LOOKING) {
            found_for_pairing(now, f, n);
        } else if (what == FIND_MASTER) {
            found_master(now, f, n);
        }
    }
    switch (K.pair_phase) {
    case PAIR_LOOKING:
        if (PAIR_BROADCAST && K.search == NULL && now - K.pair_started >= PAIR_LOOK_MS) {
            plink_addr_t b; /* nothing on mDNS: ask by broadcast (an open master answers) */
            link_subnet_broadcast(&b);
            plink_keypad_pair_start(K.k, now, &b, true, 0u);
            K.pair_phase = PAIR_ASKED;
        } else if (K.search == NULL && now >= K.next_search_at && link_wifi_up()) {
            K.next_search_at = now + PAIR_LOOK_EVERY_MS;
            search_start(FIND_PAIRING);
        }
        if (PAIR_BROADCAST == 0 && now - K.pair_started >= PAIR_GIVE_UP_MS) {
            pair_fail(UI_LINK_FAIL_NOT_FOUND); /* no alarm panel open for pairing answered */
        }
        return;
    case PAIR_ASKED: {
        plink_pair_info_t pi;
        plink_keypad_pair_info(K.k, now, &pi);
        if (pi.state == PLINK_PS_OPEN && now - K.pair_started >= PAIR_GIVE_UP_MS) {
            plink_keypad_pair_cancel(K.k, now);
            pair_fail(UI_LINK_FAIL_NOT_FOUND);
        } else if (pi.state != PLINK_PS_OPEN) {
            K.pair_phase = PAIR_NONE; /* answered: the manager's from here on */
        }
        return;
    }
    default: break;
    }
    /* Paired and not online: look for it (and again before the handshake's retries, E-18). */
    if (!K.paired || K.setup_only || info->state == PLINK_LINK_ONLINE) {
        K.misses = 0;
        return;
    }
    if (K.static_ip != 0u) {
        const plink_addr_t addr = link_addr_of(K.static_ip);
        give_master(now, &addr, false);
        return;
    }
    /* Until a first address is found, every 10 s; after that when the manager asks (resolve) */
    if (K.search == NULL && (!K.gave_addr || K.resolve) && now >= K.next_search_at &&
        link_wifi_up()) {
        K.next_search_at = now + DISCOVERY_EVERY_MS;
        K.resolve = false;
        search_start(FIND_MASTER);
    }
}

/* ---- the UI's commands ----------------------------------------------------------------------- */

static void answer(uint32_t gen, alarm_result_t r, const plink_result_t *res)
{
    portENTER_CRITICAL(&s_cmd_mux);
    s_ans.gen = gen;
    s_ans.r = r;
    if (res != NULL) {
        s_ans.res = *res;
    } else {
        memset(&s_ans.res, 0, sizeof s_ans.res);
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    xSemaphoreGive(s_cmd_done);
}

static void take_command(uint64_t now)
{
    if (K.cmd_active) {
        return;
    }
    plink_cmd_t cmd;
    uint32_t gen = 0;
    bool got = false;
    bool panic = false;
    portENTER_CRITICAL(&s_cmd_mux);
    if (s_box.pending) {
        cmd = s_box.cmd;
        gen = s_box.gen;
        s_box.pending = false;
        plink_wipe(&s_box.cmd, sizeof s_box.cmd);
        got = true;
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    if (!got) {
        return;
    }
    panic = cmd.op == PLINK_OP_COMMAND && cmd.cmd.type == ALARM_CMD_PANIC;
    const uint32_t req = plink_keypad_command(K.k, now, &cmd);
    plink_wipe(&cmd, sizeof cmd); /* the manager wipes its own once answered (LINK-018) */
    if (req == 0u) {
        answer(gen, ALARM_ERR_UNAVAILABLE, NULL); /* not online (and not a panic), or no slot */
        return;
    }
    plink_keypad_info_t info;
    plink_keypad_info(K.k, now, &info);
    if (panic && info.state != PLINK_LINK_ONLINE) {
        /* LINK-023: queued, resent every second for a minute whatever the link does - the UI
         * shows it as sent (link_info.panic_pending says it is still going) */
        answer(gen, ALARM_OK, NULL);
        plink_keypad_command_release(K.k, req);
        return;
    }
    K.cmd_active = true;
    K.cmd_gen = gen;
    K.cmd_req = req;
}

static void track_command(void)
{
    if (!K.cmd_active) {
        return;
    }
    if (atomic_load(&s_cmd_abandon) == K.cmd_gen) {
        plink_keypad_command_release(K.k, K.cmd_req); /* a panic keeps being sent (LINK-023) */
        K.cmd_active = false;
        return;
    }
    plink_result_t res;
    const plink_cmd_state_t st = plink_keypad_command_state(K.k, K.cmd_req, &res);
    if (st == PLINK_CMD_PENDING) {
        return;
    }
    if (st == PLINK_CMD_DONE) {
        answer(K.cmd_gen, (alarm_result_t)res.result, &res);
    } else {
        answer(K.cmd_gen, ALARM_ERR_UNAVAILABLE, NULL);
    }
    plink_keypad_command_release(K.k, K.cmd_req);
    K.cmd_active = false;
}

/* OFF keyed here that the master didn't run (decided 2026-10-15): it stops a fire or CO tone kept
 * through a lost link (sounds()), now or when the link goes LOST within OFF_KEEP_MS. */
static void off_not_run(void)
{
    const uint64_t now = link_now();
    portENTER_CRITICAL(&s_cmd_mux);
    s_off.set = true;
    s_off.at = now;
    portEXIT_CRITICAL(&s_cmd_mux);
}

static alarm_result_t command(const plink_cmd_t *cmd, plink_result_t *res);

alarm_result_t ksvc_command(const plink_cmd_t *cmd, plink_result_t *res)
{
    memset(res, 0, sizeof *res);
    if (K.k == NULL || K.setup_only || s_cmd_mutex == NULL) {
        return ALARM_ERR_UNAVAILABLE;
    }
    const alarm_result_t r = command(cmd, res);
    /* UNAVAILABLE: refused at once without the link, or no answer in 3 s. A refusal the master
     * sent (a wrong code) doesn't count: the master is there and decides. */
    if (r == ALARM_ERR_UNAVAILABLE && cmd->op == PLINK_OP_COMMAND &&
        (cmd->cmd.type == ALARM_CMD_DISARM || cmd->cmd.type == ALARM_CMD_SILENCE)) {
        off_not_run();
    }
    return r;
}

static alarm_result_t command(const plink_cmd_t *cmd, plink_result_t *res)
{
    const bool panic = cmd->op == PLINK_OP_COMMAND && cmd->cmd.type == ALARM_CMD_PANIC;
    if (!panic) {
        link_pub_lock();
        const bool online = g_pub.kinfo_valid && g_pub.kraw.state == PLINK_LINK_ONLINE;
        link_pub_unlock();
        if (!online) {
            return ALARM_ERR_UNAVAILABLE; /* at once: never a frozen screen (E-06) */
        }
    }
    xSemaphoreTake(s_cmd_mutex, portMAX_DELAY);
    (void)xSemaphoreTake(s_cmd_done, 0); /* the answer to one given up on */
    portENTER_CRITICAL(&s_cmd_mux);
    s_box.cmd = *cmd;
    const uint32_t gen = ++s_cmd_gen;
    s_box.gen = gen;
    s_box.pending = true;
    portEXIT_CRITICAL(&s_cmd_mux);
    alarm_result_t r = ALARM_ERR_UNAVAILABLE;
    if (xSemaphoreTake(s_cmd_done, pdMS_TO_TICKS(CMD_WAIT_MS)) == pdTRUE) {
        portENTER_CRITICAL(&s_cmd_mux);
        if (s_ans.gen == gen) {
            r = s_ans.r;
            *res = s_ans.res;
        }
        portEXIT_CRITICAL(&s_cmd_mux);
    } else {
        atomic_store(&s_cmd_abandon, gen);
        bool withdrawn = false;
        portENTER_CRITICAL(&s_cmd_mux);
        if (s_box.pending && s_box.gen == gen) {
            s_box.pending = false;
            plink_wipe(&s_box.cmd, sizeof s_box.cmd);
            withdrawn = true;
        }
        portEXIT_CRITICAL(&s_cmd_mux);
        /* A panic the link took is still being sent for a minute (link_info.panic_pending) */
        r = panic && !withdrawn ? ALARM_OK : ALARM_ERR_UNAVAILABLE;
    }
    xSemaphoreGive(s_cmd_mutex);
    return r;
}

/* ---- sounds, time, power, the heartbeat ------------------------------------------------------ */

/* The keypad's continuous pattern: fire, then CO, from any partition; else its home partition's. */
static uint8_t continuous_of(const alarm_snapshot_t *s, uint8_t home)
{
    bool fire = false;
    bool co = false;
    for (uint8_t p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) != 0u) {
            fire = fire || s->part[p].sounder == ALARM_SOUND_FIRE;
            co = co || s->part[p].sounder == ALARM_SOUND_CO;
        }
    }
    if (fire) {
        return ALARM_SOUND_FIRE;
    }
    if (co) {
        return ALARM_SOUND_CO;
    }
    return home >= 1u && home <= ALARM_MAX_PARTITIONS ? (uint8_t)s->part[home - 1u].sounder
                                                      : (uint8_t)ALARM_SOUND_NONE;
}

static void sounds(uint64_t now, const plink_keypad_info_t *info)
{
    plink_sound_t s;
    while (plink_keypad_take_sound(K.k, &s)) {
        sounder_play((alarm_sound_t)s.sound);
    }
    const bool online = info->state == PLINK_LINK_ONLINE;
    bool off = false;
    portENTER_CRITICAL(&s_cmd_mux);
    if (s_off.set && now >= s_off.at + OFF_KEEP_MS) {
        s_off.set = false; /* too old to be about this loss */
    }
    if (s_off.set && !online) {
        s_off.set = false;
        off = true;
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    if (off) {
        K.local_off = true;
    }
    uint8_t want = ALARM_SOUND_NONE;
    if (online) {
        K.local_off = false; /* the master's state again */
        want = continuous_of(plink_keypad_snapshot(K.k), info->home);
    } else if ((K.cont == ALARM_SOUND_FIRE || K.cont == ALARM_SOUND_CO) && !K.local_off) {
        want = K.cont; /* no stale entry or burglary tone, but fire and CO go on */
    }
    if (want != K.cont) {
        K.cont = want;
        sounder_set_continuous((alarm_sound_t)want);
    }
}

static void follow_time(uint64_t now)
{
    plink_time_t t;
    if (plink_keypad_take_time(K.k, &t)) {
        const char *tz = getenv("TZ");
        if (t.tz[0] != '\0' && (tz == NULL || strcmp(tz, t.tz) != 0) &&
            net_wifi_set_timezone(t.tz) == ESP_OK) {
            ESP_LOGI(TAG, "time zone from the alarm panel: %s", t.tz);
        }
        K.clock_at = 0u; /* and the clock at once */
    }
    if (now < K.clock_at) {
        return;
    }
    K.clock_at = now + CLOCK_CHECK_MS;
    const int64_t master = plink_keypad_epoch_ms(K.k, now); /* 0: not known, never applied */
    if (master <= 0) {
        return;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t mine = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (llabs(master - mine) > CLOCK_STEP_MS) {
        const struct timeval set = {.tv_sec = (time_t)(master / 1000),
                                    .tv_usec = (suseconds_t)((master % 1000) * 1000)};
        settimeofday(&set, NULL);
        net_wifi_clock_set();
        ESP_LOGI(TAG, "clock set from the alarm panel (it was %lld ms off)",
                 (long long)(master - mine));
    }
}

static void network(uint64_t now)
{
    if (now < K.net_at) {
        return;
    }
    K.net_at = now + NET_CHECK_MS;
    net_wifi_status_t st;
    net_wifi_get_status(&st);
    const bool up = st.state == NET_WIFI_CONNECTED;
    if (up && strcmp(st.ip, K.ip) != 0) {
        /* A new address, also across a drop (the usual way it changes): a HEARTBEAT at once from
         * it, so the master's STATUS follows it (nothing without a live session). */
        if (K.ip[0] != '\0') {
            plink_keypad_addr_changed(K.k, now);
        }
        snprintf(K.ip, sizeof K.ip, "%s", st.ip);
    }
    if (up && !K.net_up) {
        K.next_search_at = now; /* a network (back): look for the master now */
    }
    K.net_up = up;
    plink_keypad_set_power(K.k, (uint8_t)atomic_load(&s_power_flags), up ? st.rssi : 0);
    /* LINK-020 / docs/features/11 "Time": no time server of its own while paired */
    if (K.paired != K.sntp_off) {
        K.sntp_off = K.paired;
        net_wifi_set_time_server(!K.paired);
    }
}

/* The link's heartbeat for the update's self-test (PANEL_OTA_OK_LINK): authenticated frames are
 * coming (a status under 15 s old), or the keypad isn't paired (nothing to hear). */
static void beat(uint64_t now, const plink_keypad_info_t *info)
{
    if (now < K.beat_at) {
        return;
    }
    K.beat_at = now + BEAT_MS;
    if (info->state == PLINK_LINK_NOT_PAIRED || info->status_age_s <= LINK_STATUS_FRESH ||
        atomic_load(&s_maint_beat)) {
        panel_ota_healthy(PANEL_OTA_OK_LINK);
    }
}

/* The maintenance update's mark, gone once its image is confirmed or the link is back. */
static void maintenance_mark(uint64_t now, const plink_keypad_info_t *info)
{
    if (!atomic_load(&s_maint_marked) || now < s_maint_at) {
        return;
    }
    s_maint_at = now + MAINT_CHECK_MS;
    bool done = info->state == PLINK_LINK_ONLINE;
    if (!done && atomic_load(&s_maint_beat)) {
        panel_ota_status_t st; /* the update's probation: over once confirmed (or rolled back) */
        panel_ota_get_status(&st);
        done = !st.probation;
    }
    if (done && link_nvs_flag(LINK_KEY_MAINT, false) == ESP_OK) {
        atomic_store(&s_maint_beat, false);
        atomic_store(&s_maint_marked, false);
    }
}

void plink_service_keypad_power(bool mains_lost, bool battery_low, bool battery_present)
{
    atomic_store(&s_power_flags, (mains_lost ? PLINK_HB_MAINS_LOST : 0u) |
                                     (battery_low ? PLINK_HB_BATTERY_LOW : 0u) |
                                     (battery_present ? PLINK_HB_BATTERY_THERE : 0u));
}

/* ---- what the UI, the console and the update gate read --------------------------------------- */

static void pair_status(uint64_t now, ui_link_pair_t *out, bool *valid)
{
    memset(out, 0, sizeof *out);
    const uint32_t looking = K.pair_started != 0u ? (uint32_t)((now - K.pair_started) / 1000u) : 0u;
    if (K.pair_phase == PAIR_FAILED) {
        *valid = true;
        out->state = UI_LINK_PAIR_FAILED;
        out->fail = K.pair_fail;
        out->left_s = looking;
        return;
    }
    if (K.pair_phase == PAIR_LOOKING) {
        *valid = true;
        out->state = UI_LINK_PAIR_OPEN;
        out->left_s = looking;
        return;
    }
    plink_pair_info_t pi;
    plink_keypad_pair_info(K.k, now, &pi);
    *valid = pi.state != PLINK_PS_IDLE;
    out->state = pi.state;
    out->fail = pi.fail;
    out->attempts = pi.attempts;
    out->left_s = looking; /* a keypad: looking for this long (ui_backend.h) */
    snprintf(out->code, sizeof out->code, "%s", pi.code);
    snprintf(out->peer_name, sizeof out->peer_name, "%s", pi.peer_name);
    link_addr_ip(&pi.peer, out->peer_addr);
    out->device = pi.device;
}

/* The home partition's name for the setup's last step on a master (Option C), which pulls no
 * configuration: a master names itself after partition 1 (LINK_DEFAULT_NAME while it has none), so
 * for home 1 its name is the partition's. "" otherwise: the step says "Partition 2". */
static void setup_home_name(const plink_keypad_rec_t *rec, char *out, size_t len)
{
    out[0] = '\0';
    if (rec->home == 1u && rec->master_name[0] != '\0' &&
        strcmp(rec->master_name, LINK_DEFAULT_NAME) != 0) {
        snprintf(out, len, "%s", rec->master_name);
    }
}

/* needs_update (ui_link_info_t): a configuration this keypad couldn't read is the master's
 * current one. The manager keeps the failed CRC after a later configuration installs, so one seen
 * before the latest install no longer counts while the link is up. */
static bool needs_update(const plink_keypad_info_t *info)
{
    const uint32_t cfg_seq = plink_keypad_config_seq(K.k);
    if (info->cfg_bad_crc != K.bad_crc) {
        K.bad_crc = info->cfg_bad_crc;
        K.bad_cfg_seq = cfg_seq;
    }
    if (K.bad_crc == 0u) {
        return false;
    }
    return info->state != PLINK_LINK_ONLINE || cfg_seq == K.bad_cfg_seq;
}

static void publish(uint64_t now, const plink_keypad_info_t *info)
{
    ui_link_pair_t pair;
    bool pair_valid = false;
    pair_status(now, &pair, &pair_valid);
    if (K.setup_only) {
        char master_addr[24] = "";
        if (K.pending && info->master.tp == PLINK_TP_UDP) {
            link_addr_ip(&info->master, master_addr); /* where the pairing found it */
        }
        char home_name[PLINK_NAME_MAX + 1] = "";
        if (K.pending) {
            setup_home_name(&K.pending_rec, home_name, sizeof home_name);
        }
        link_pub_lock();
        g_pub.setup_keypad = true;
        g_pub.pair_valid = pair_valid;
        g_pub.pair = pair;
        g_pub.setup_paired = K.pending;
        g_pub.setup_device = K.pending ? K.pending_rec.device : 0u;
        g_pub.setup_home = K.pending ? K.pending_rec.home : 0u;
        snprintf(g_pub.setup_master, sizeof g_pub.setup_master, "%s",
                 K.pending ? K.pending_rec.master_name : "");
        snprintf(g_pub.setup_master_addr, sizeof g_pub.setup_master_addr, "%s", master_addr);
        snprintf(g_pub.setup_home_name, sizeof g_pub.setup_home_name, "%s", home_name);
        link_pub_unlock();
        return;
    }
    if (info->home != K.last_home) {
        K.last_home = info->home;
        K.home_gen++; /* the UI checks the new home on its own task (docs/features/11 "Roles") */
    }
    const int64_t epoch = link_epoch_ms();
    ui_link_info_t li;
    memset(&li, 0, sizeof li);
    li.role = UI_LINK_ROLE_KEYPAD;
    li.state = info->state;
    li.removed =
        info->state == PLINK_LINK_NOT_PAIRED && info->unpaired_why == PLINK_UNPAIRED_REMOVED;
    li.maintenance = info->maintenance;
    li.needs_update = needs_update(info);
    li.panic_pending = plink_keypad_panic_pending(K.k);
    /* a fire or CO tone kept through a lost link: Home offers Silence (sounds() keeps it) */
    li.local_alarm = info->state != PLINK_LINK_ONLINE &&
                     (K.cont == ALARM_SOUND_FIRE || K.cont == ALARM_SOUND_CO) && !K.local_off;
    const alarm_config_t *cfg = plink_keypad_config(K.k);
    if (cfg != NULL && info->home >= 1u && info->home <= ALARM_MAX_PARTITIONS) {
        snprintf(li.home_name, sizeof li.home_name, "%.*s", (int)(sizeof li.home_name - 1u),
                 cfg->partitions[info->home - 1u].name);
    }
    li.device = info->device;
    li.home = info->home;
    li.home_gen = K.home_gen;
    li.since_epoch_ms = (info->status_age_s != UINT32_MAX && epoch != 0)
                            ? epoch - (int64_t)info->status_age_s * 1000
                            : 0;
    snprintf(li.master_name, sizeof li.master_name, "%s", info->master_name);
    if (info->master.tp == PLINK_TP_UDP) {
        link_addr_ip(&info->master, li.master_addr); /* About shows it; "" not known */
    }
    /* The master's reporting for the Emergency words (UI-029): what it said while this keypad can
     * reach it; set up but out of reach otherwise. */
    uint8_t report = plink_keypad_report(K.k);
    if (info->state != PLINK_LINK_ONLINE && report != PLINK_REPORT_NONE) {
        report = PLINK_REPORT_OFFLINE;
    }
    link_pub_lock();
    g_pub.kinfo = li;
    g_pub.kraw = *info;
    g_pub.kreport = report;
    g_pub.kinfo_valid = true;
    g_pub.pair_valid = pair_valid;
    g_pub.pair = pair;
    link_pub_unlock();
}

/* The status, configuration and events for the UI. The configuration goes before the snapshot
 * that counts it, in one hold of the lock: a UI that sees the new config_seq gets the new
 * configuration. */
static void copy_for_ui(uint64_t now)
{
    const uint32_t seq = plink_keypad_seq(K.k);
    const uint32_t cfg_seq = plink_keypad_config_seq(K.k);
    const alarm_config_t *cfg = plink_keypad_config(K.k);
    const bool snap_new = !K.copied || seq != K.seq_copied;
    const bool cfg_new = cfg != NULL && (!K.copied || cfg_seq != K.cfg_seq_copied);
    const bool events_due = snap_new || now >= K.events_at;
    if (!snap_new && !cfg_new && !events_due) {
        return;
    }
    size_t n = 0;
    if (events_due) {
        K.events_at = now + PUBLISH_MS;
        n = plink_keypad_events(K.k, s_ev_tmp, PLINK_EVENTS_KEPT);
    }
    link_pub_lock();
    if (cfg_new) {
        memcpy(s_ui_cfg, cfg, sizeof *s_ui_cfg);
        s_ui_have_cfg = true;
    }
    if (snap_new) {
        memcpy(s_ui_snap, plink_keypad_snapshot(K.k), sizeof *s_ui_snap);
        s_ui_seq = seq;
    }
    if (events_due) {
        memcpy(s_ui_ev, s_ev_tmp, n * sizeof *s_ui_ev);
        s_ui_ev_n = n;
    }
    link_pub_unlock();
    K.copied = true;
    K.seq_copied = seq;
    if (cfg_new) {
        K.cfg_seq_copied = cfg_seq;
    }
}

uint32_t ksvc_ui_seq(void)
{
    link_pub_lock();
    const uint32_t seq = s_ui_seq;
    link_pub_unlock();
    return seq;
}

void ksvc_ui_snapshot(alarm_snapshot_t *out)
{
    if (s_ui_snap == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    link_pub_lock();
    memcpy(out, s_ui_snap, sizeof *out);
    link_pub_unlock();
}

bool ksvc_ui_config(alarm_config_t *out)
{
    if (s_ui_cfg == NULL) {
        return false;
    }
    link_pub_lock();
    /* a keypad's: the setup's pairing on a master never serves another master's configuration */
    const bool have = s_ui_have_cfg && !g_pub.setup_keypad;
    if (have) {
        memcpy(out, s_ui_cfg, sizeof *out);
    }
    link_pub_unlock();
    return have;
}

size_t ksvc_ui_events(alarm_event_t *out, size_t max)
{
    if (s_ui_ev == NULL) {
        return 0u;
    }
    link_pub_lock();
    const size_t n = s_ui_ev_n < max ? s_ui_ev_n : max;
    memcpy(out, s_ui_ev, n * sizeof *out);
    link_pub_unlock();
    return n;
}

bool plink_service_update_idle(void *ctx, const char **why)
{
    (void)ctx;
    static const char *const NO_LINK = I18N_NOOP_C("update", "No system communication");
    bool idle = false;
    bool maintenance = false;
    const char *reason = NO_LINK;
    link_pub_lock();
    if (g_pub.kinfo_valid && s_ui_snap != NULL) {
        const plink_keypad_info_t *i = &g_pub.kraw;
        const bool fresh = i->status_age_s <= LINK_STATUS_FRESH;
        if (i->state == PLINK_LINK_ONLINE && fresh) {
            idle = proto_fw_install_allowed(s_ui_snap, &reason); /* the master's partitions */
        } else if (i->state != PLINK_LINK_NOT_PAIRED && i->cfg_bad_crc != 0u && fresh) {
            /* It hears its master (statuses come, the snapshot follows them) but can't read the
             * master's configuration, so it never gets ONLINE: this update is its way out, under
             * the same rule as ONLINE's. */
            idle = proto_fw_install_allowed(s_ui_snap, &reason);
        } else if (i->state == PLINK_LINK_NOT_PAIRED || i->maintenance) {
            idle = true; /* nothing to protect here, or LINK-025's recovery */
            maintenance = i->state != PLINK_LINK_NOT_PAIRED;
            reason = "";
        }
    }
    link_pub_unlock();
    /* An update through the maintenance can't prove the link after its restart: mark it first,
     * so the new image's self-test isn't failed for a master that is out of reach. One NVS write
     * (this runs on the update or console task, both with internal-RAM stacks). */
    if (maintenance && !atomic_load(&s_maint_marked)) {
        if (link_nvs_flag(LINK_KEY_MAINT, true) == ESP_OK) {
            atomic_store(&s_maint_marked, true);
        } else {
            idle = false;
            reason = NO_LINK;
        }
    }
    if (why != NULL) {
        *why = reason;
    }
    return idle;
}

/* ---- the link task --------------------------------------------------------------------------- */

void ksvc_input(uint64_t now, const plink_addr_t *from, const uint8_t *frame, size_t len)
{
    plink_keypad_input(K.k, now, from, frame, len);
}

/* Option C once paired: the manager rests until the setup's Done (no handshake, port_send), so
 * nothing of it is due - its handshake timers would otherwise wake the link task for nothing. */
static bool setup_resting(void)
{
    return K.setup_only && K.pending;
}

uint64_t ksvc_next(void)
{
    /* resting, the link task's own 50 ms turn is enough for its publishing */
    return K.k != NULL && !setup_resting() ? plink_keypad_next(K.k) : UINT64_MAX;
}

void ksvc_poll(uint64_t now)
{
    if (K.k == NULL) {
        return;
    }
    if (!K.setup_only) {
        take_command(now);
    }
    plink_keypad_info_t info;
    plink_keypad_info(K.k, now, &info);
    discovery(now, &info);
    if (!setup_resting()) {
        plink_keypad_tick(K.k, now);
    }
    plink_keypad_info(K.k, now, &info);
    if (!K.setup_only) {
        track_command();
        network(now);
        sounds(now, &info);
        follow_time(now);
        beat(now, &info);
        maintenance_mark(now, &info);
    }
    copy_for_ui(now);
    if (now >= K.publish_at) {
        K.publish_at = now + PUBLISH_MS;
        publish(now, &info);
    }
}

uint8_t ksvc_request(uint64_t now, const link_req_t *req)
{
    if (K.setup_only && K.pending &&
        (req->kind == UI_LINK_REQ_KP_PAIR_START || req->kind == LINK_REQ_KP_PAIR_AT ||
         req->kind == UI_LINK_REQ_KP_PAIR_RETRY)) {
        /* Option C: a new attempt replaces a pairing that finished but wasn't kept (its STOP
         * never came): forgotten first, or port_send would drop the new attempt's frames */
        (void)plink_keypad_forget(K.k, now);
    }
    switch (req->kind) {
    case UI_LINK_REQ_KP_PAIR_START:
    case LINK_REQ_KP_PAIR_AT:
        pair_begin(now, req->kind == LINK_REQ_KP_PAIR_AT ? req->ip : 0u);
        break;
    case UI_LINK_REQ_KP_PAIR_CONFIRM: plink_keypad_pair_confirm(K.k, now, req->allow); break;
    case UI_LINK_REQ_KP_PAIR_RETRY:
        if (K.pair_phase == PAIR_FAILED) {
            pair_begin(now, 0u); /* our own failure (not found, two panels): look again */
        } else {
            plink_keypad_pair_retry(K.k, now);
            K.pair_started = now;
        }
        break;
    case UI_LINK_REQ_KP_PAIR_STOP:
        if (K.setup_only) {
            ksvc_setup_end(now); /* the wizard left the keypad branch: nothing of it stays */
            return UI_LINK_REQ_OK;
        }
        pair_end();
        plink_keypad_pair_cancel(K.k, now);
        break;
    case UI_LINK_REQ_KP_DONE:
        if (K.setup_only) {
            return UI_LINK_REQ_REFUSED; /* the master's request handles it (ksvc_setup_done) */
        }
        /* a keypad already: nothing to store but the setup's end */
        if (panel_setup_set_done() != ESP_OK) {
            return UI_LINK_REQ_FAILED;
        }
        break;
    case LINK_REQ_KP_STATIC: {
        if (K.setup_only) {
            return UI_LINK_REQ_REFUSED;
        }
        const esp_err_t err = link_nvs_u32(LINK_KEY_STATIC, req->ip, req->ip == 0u);
        if (err != ESP_OK) {
            return UI_LINK_REQ_FAILED;
        }
        K.static_ip = req->ip;
        K.gave_addr = false;
        K.next_search_at = now;
        break;
    }
    case LINK_REQ_KP_FORGET:
        if (K.setup_only) {
            return UI_LINK_REQ_REFUSED;
        }
        pair_end();
        if (!plink_keypad_forget(K.k, now)) {
            return UI_LINK_REQ_FAILED;
        }
        break;
    default: return UI_LINK_REQ_REFUSED; /* the master's requests */
    }
    plink_keypad_info_t info;
    plink_keypad_info(K.k, now, &info);
    publish(now, &info);
    return UI_LINK_REQ_OK;
}

bool ksvc_setup_done(void)
{
    if (K.k == NULL || !K.setup_only || !K.pending) {
        return false;
    }
    uint8_t buf[PLINK_STORE_MAX];
    size_t len = 0;
    esp_err_t err = plink_store_keypad_encode(&K.pending_rec, buf, sizeof buf, &len)
                        ? link_nvs_write(LINK_KEY_KEYPAD, buf, len)
                        : ESP_FAIL;
    plink_wipe(buf, sizeof buf);
    if (err == ESP_OK) {
        err = link_store_role(PLINK_SERVICE_KEYPAD);
    }
    if (err == ESP_OK) {
        err = panel_setup_set_done(); /* the keypad starts on its Home, not the setup again */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "becoming a keypad couldn't be stored (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "paired as keypad %u: restarting as a keypad", (unsigned)K.pending_rec.device);
    plink_wipe(&K.pending_rec, sizeof K.pending_rec);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return true;
}

/* The keypad's stored record (NULL buffer: just its home). */
static bool load_record(plink_keypad_rec_t *rec)
{
    uint8_t buf[PLINK_STORE_MAX];
    size_t len = 0;
    bool have = false;
    if (link_nvs_read(LINK_KEY_KEYPAD, buf, sizeof buf, &len)) {
        have = plink_store_keypad_decode(buf, len, rec);
        if (!have) {
            ESP_LOGE(TAG, "the stored master's record can't be read: not paired");
        }
    }
    plink_wipe(buf, sizeof buf);
    return have;
}

uint8_t plink_service_keypad_home(void)
{
    static plink_keypad_rec_t rec ALARM_SLOW_BSS; /* the start-up's, wiped at once */
    uint8_t home = 1u;
    if (load_record(&rec) && rec.home >= 1u && rec.home <= ALARM_MAX_PARTITIONS) {
        home = rec.home;
    }
    plink_wipe(&rec, sizeof rec);
    return home;
}

void ksvc_setup_end(uint64_t now)
{
    if (K.k == NULL || !K.setup_only) {
        return;
    }
    search_stop();
    plink_keypad_pair_cancel(K.k, now); /* an attempt under way ends on both screens */
    /* A pairing that finished meanwhile (the UI sees DONE up to 1.5 s late) is forgotten: its
     * record and keys wiped (port_store with NULL). The other master heard nothing after its
     * PAIR_DONE (port_send) and drops the keypad itself after 10 minutes (LINK-016). */
    (void)plink_keypad_forget(K.k, now);
    plink_wipe(K.k, sizeof *K.k);
    plink_wipe(K.cfg_buf, ALARM_CONFIG_BLOB_MAX);
    plink_wipe(K.cfg, sizeof *K.cfg);
    plink_wipe(&K.pending_rec, sizeof K.pending_rec);
    link_pub_lock();
    g_pub.setup_keypad = false; /* the master's own pairing is published again */
    g_pub.setup_paired = false;
    g_pub.setup_device = 0u;
    g_pub.setup_home = 0u;
    g_pub.setup_master[0] = '\0';
    g_pub.setup_master_addr[0] = '\0';
    g_pub.setup_home_name[0] = '\0';
    g_pub.pair_valid = false;
    memset(&g_pub.pair, 0, sizeof g_pub.pair);
    s_ui_have_cfg = false;
    s_ui_ev_n = 0u;
    s_ui_seq = 0u;
    plink_wipe(s_ui_cfg, sizeof *s_ui_cfg);
    plink_wipe(s_ui_snap, sizeof *s_ui_snap);
    plink_wipe(s_ui_ev, PLINK_EVENTS_KEPT * sizeof *s_ui_ev);
    link_pub_unlock();
    memset(&K, 0, sizeof K); /* gone: ksvc_active() false, its frames no longer routed here */
    plink_psa_forget();
    ESP_LOGI(TAG, "the setup's pairing as a keypad ended");
}

esp_err_t ksvc_prepare(uint64_t now, bool setup_only)
{
    /* allocated once, kept when the setup's pairing ends (ksvc_setup_end) */
    if (s_kmem == NULL) {
        s_kmem = alarm_mem_slow_calloc(1, sizeof *s_kmem);
    }
    if (s_cfg_buf_mem == NULL) {
        s_cfg_buf_mem = alarm_mem_slow_malloc(ALARM_CONFIG_BLOB_MAX);
    }
    if (s_cfg_mem == NULL) {
        s_cfg_mem = alarm_mem_slow_calloc(1, sizeof *s_cfg_mem);
    }
    if (s_kmem == NULL || s_cfg_buf_mem == NULL || s_cfg_mem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* the UI's copies (the setup's pairing on a master leaves them empty) */
    if (s_ui_snap == NULL) {
        s_ui_snap = alarm_mem_slow_calloc(1, sizeof *s_ui_snap);
    }
    if (s_ui_cfg == NULL) {
        s_ui_cfg = alarm_mem_slow_calloc(1, sizeof *s_ui_cfg);
    }
    if (s_ui_ev == NULL) {
        s_ui_ev = alarm_mem_slow_calloc(PLINK_EVENTS_KEPT, sizeof *s_ui_ev);
    }
    if (s_ev_tmp == NULL) {
        s_ev_tmp = alarm_mem_slow_calloc(PLINK_EVENTS_KEPT, sizeof *s_ev_tmp);
    }
    if (s_ui_snap == NULL || s_ui_cfg == NULL || s_ui_ev == NULL || s_ev_tmp == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&K, 0, sizeof K);
    K.setup_only = setup_only;
    plink_keypad_t *k = s_kmem;
    K.cfg_buf = s_cfg_buf_mem;
    K.cfg = s_cfg_mem;
    if (!setup_only) {
        s_cmd_mutex = xSemaphoreCreateMutex();
        s_cmd_done = xSemaphoreCreateBinary();
        if (s_cmd_mutex == NULL || s_cmd_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    static plink_keypad_rec_t rec ALARM_SLOW_BSS; /* the start-up's, wiped after init */
    const bool have = !setup_only && load_record(&rec);
    remember_master(have ? &rec : NULL);
    K.last_home = K.home;
    if (!setup_only && !link_nvs_get_u32(LINK_KEY_STATIC, &K.static_ip)) {
        K.static_ip = 0u;
    }
    if (!setup_only && link_nvs_get_flag(LINK_KEY_MAINT)) {
        /* restarted from an update the maintenance let through: its self-test needs the beat */
        atomic_store(&s_maint_marked, true);
        atomic_store(&s_maint_beat, true);
        ESP_LOGW(TAG, "updated through the maintenance: the link counts as running until the "
                      "update is confirmed or the alarm panel answers");
    }
    const plink_caps_t caps = {.partitions = ALARM_MAX_PARTITIONS,
                               .zones = ALARM_MAX_ZONES,
                               .users = ALARM_MAX_USERS,
                               .devices = ALARM_MAX_DEVICES};
    /* The name it gives itself when pairing, whole and short (the master's list shows it until
     * someone renames it there): its panel id tells two new keypads apart, and nothing says what
     * hardware it is (the screens never do). */
    char name[32];
    snprintf(name, sizeof name, "Keypad %s", g_link.panel_id);
    static const plink_keypad_port_t port = {
        .ctx = NULL, .send = port_send, .mtu = NULL, .store = port_store, .resolve = port_resolve};
    plink_keypad_init(k, &g_link.crypto, &port, have ? &rec : NULL, K.cfg_buf,
                      ALARM_CONFIG_BLOB_MAX, K.cfg, PLINK_KIND_PANEL, &caps, name, g_link.fw, now);
    plink_wipe(&rec, sizeof rec);
    K.k = k;
    K.next_search_at = now;
    if (!setup_only) {
        /* LINK-024: a keypad answers within a second; and no time server of its own while paired */
        net_wifi_hold_low_latency(NET_WIFI_LATENCY_LINK, true);
        K.sntp_off = K.paired;
        net_wifi_set_time_server(!K.paired);
        ESP_LOGI(TAG, "keypad: %s", K.paired ? "paired" : "not paired");
    }
    plink_keypad_info_t info;
    plink_keypad_info(K.k, now, &info);
    publish(now, &info);
    return ESP_OK;
}

#endif /* CONFIG_ALARM_LINK */
