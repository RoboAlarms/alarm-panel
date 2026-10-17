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
 *   - the voice (LINK-028): the master's events, sounds and status handed to the keypad's own
 *     voice through plink_vfeed (covered events only, nothing older than its last baseline,
 *     nothing while the link isn't ONLINE), as the alarm task hands an alarm panel's;
 *   - the time: the master's clock and zone, and no time server of its own while paired;
 *   - the update gate and the link's heartbeat for the update's self-test (LINK-020);
 *   - Smart Home (BL-216; docs/features/11 "Smart Home and Home Assistant on keypads"): the core's
 *     mirror of the master's devices (PSRAM, offered as PLINK_CAP_HOME only with it) and the
 *     screen's ui_dev_t copy made from it under s_dev_lock (never link_pub_lock), each tile
 *     offline since the last device data while that isn't fresh (LINK-046); the screen's device
 *     actions (ksvc_dev_act, a hand-over of their own, never the commands'), waited for 1 s (3 s
 *     with a code), then shown pending until the answer or the give-up (LINK-047);
 *   - its settings (PLINK_CAP_PREFS, with the UI): reported on every save (panel_prefs_seq) and
 *     voice change, and a change from the master (PREFS_SET) checked here, handed to the screen
 *     through panel_prefs' mailbox (UI-046) and answered with the values then stored (LINK-050,
 *     LINK-051): the link task never takes board_ui_lock and never writes NVS for it.
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
#include "panel_link/plink_vfeed.h"
#include "panel_link/plink_view.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "panel_voice.h"
#include "sounder.h"
#include "ui_backend/ui_dev_model.h"
#include "ui_backend/ui_prefs.h"

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
/* Smart Home (docs/features/11 "Device actions"): the screen's wait for a device action's answer,
 * 3 s with a code (the master checks it on its alarm task); then the link task shows it pending
 * and says so, within this. */
#define DEV_WAIT_MS        1000u
#define DEV_WAIT_CODE_MS   3000u
#define DEV_RELEASE_MS     150u
/* Settings (LINK-050, LINK-051): the voice's state looked at this often for the report; a change
 * the screen took is answered by it within 2 s (UI_PREFS_LOOK_MS), and this is the safety net. */
#define PREFS_VOICE_MS     1000u
#define PSET_LOST_MS       10000u

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
    /* the voice (LINK-028; only with a voice set, the keypad role) */
    bool voiced;
    plink_vfeed_t vf;
    /* Smart Home: the screen's device action in flight (one at a time), */
    bool act_active;
    uint32_t act_gen;
    uint32_t act_ticket;
    /* ... its answer, given once the screen's copy shows what it did */
    bool act_due;
    uint32_t act_due_gen;
    uint8_t act_res, act_why;
    bool act_released;
    /* ... and the copy the screen reads: made again when the mirror or its freshness moves */
    bool dev_copied;
    uint32_t dev_seq_copied;
    bool dev_fresh_copied;
    /* settings (PLINK_CAP_PREFS): the report, */
    bool prefs_on;
    bool prefs_reported;
    uint32_t prefs_seq;
    uint8_t voice, prefs_voice;      /* plink_voice_t */
    uint32_t accepts, prefs_accepts; /* proto_prefs_mask_t */
    uint64_t voice_at;
    /* ... and a change from the master on its way through the screen */
    bool pset_active;
    bool pset_cancel_tried;
    uint32_t pset_req;
    uint32_t pset_ticket; /* the mailbox's */
    uint32_t pset_have;   /* the fields handed to the screen */
    uint64_t pset_since;
    uint8_t pset_n; /* refusals so far: (id, why) pairs in s_pset_refused */
    /* the rest */
    uint64_t clock_at, net_at, publish_at, beat_at, events_at;
    char ip[16]; /* the last address it had: kept across a drop, so a new one is told at once */
    bool net_up;
    bool sntp_off;
    uint8_t last_home;
    uint32_t home_gen;
    bool copied;
    uint32_t seq_copied, cfg_seq_copied;
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
static uint32_t s_ui_cfg_seq; /* the copy's config_seq (the voice's words follow it) */
static size_t s_ui_ev_n;

/* The keypad's voice (LINK-028): its producer entries, set before the link starts. */
static const plink_service_voice_t *s_voice;
static atomic_bool s_voice_resync;

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

/* Smart Home (BL-216): all PSRAM, allocated once at the start (ksvc_prepare), only with home
 * control and PSRAM; s_home says they all are, set before the link task and the UI start. */
static bool s_home;
static plink_dev_mirror_t *s_mirror; /* the core's (about 31 KB) */
/* The screen's copy (s_dev_lock): the rows as ui_dev_from_parts makes them, whether the device
 * data is fresh, and the mirror's state for the console (as of the link task's last turn). */
static SemaphoreHandle_t s_dev_lock;
static ui_dev_t *s_kdevs; /* PLINK_DEV_MAX */
static size_t s_kdev_n;
static bool s_kdev_fresh;
static plink_dev_info_t s_kdev_info ALARM_SLOW_BSS;
static uint64_t s_kdev_at; /* link_now() of the rows' copy */
/* The screen's device actions (ksvc_dev_act): one at a time, their own hand-over - never the
 * commands' s_cmd_mutex or slots - the code wiped once handed over. */
static SemaphoreHandle_t s_act_mutex;
static SemaphoreHandle_t s_act_done;
static portMUX_TYPE s_act_mux = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    char ref[PLINK_REF_MAX + 1];
    uint8_t action;
    int32_t value;
    uint8_t mode; /* ACT_SET_MODE: its DEV_MODE_* bit */
    char code[ALARM_CODE_MAX_LEN + 1];
} dev_ask_t;
static struct {
    bool pending;
    uint32_t gen;
    dev_ask_t ask;
} s_abox ALARM_SLOW_BSS; /* PSRAM, as the master's device jobs: internal RAM is the reserve */
typedef struct {
    uint32_t gen;
    uint8_t result; /* plink_dev_result_t */
    uint8_t why;    /* CODE_REFUSED: the alarm_result_t */
    bool released;  /* not answered in time: shown pending (or "Not confirmed") on its tile */
} dev_ans_t;
static dev_ans_t s_aans;
static uint32_t s_act_gen;
static atomic_uint s_act_abandon; /* the gen the screen stopped waiting for */

/* A settings change's refusals so far, (id, why) pairs (the link task's). */
static uint8_t s_pset_refused[2u * PLINK_PREFS_REFUSED_MAX] ALARM_SLOW_BSS;
/* A settings change's answer from the screen's task (panel_prefs_done_fn), for the link task. */
static portMUX_TYPE s_pset_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool set;
    uint32_t ticket;
    uint8_t why[PROTO_PREFS_F_LAST];
} s_pset_ans;

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

/* ---- Smart Home: the master's devices on this keypad's screen (BL-216) ----------------------- */

_Static_assert((int)PLINK_DEV_OK == (int)UI_DEV_OK &&
                   (int)PLINK_DEV_NEED_CODE == (int)UI_DEV_NEED_CODE &&
                   (int)PLINK_DEV_CODE_REFUSED == (int)UI_DEV_CODE_REFUSED &&
                   (int)PLINK_DEV_BUSY == (int)UI_DEV_BUSY &&
                   (int)PLINK_DEV_UNREACHABLE == (int)UI_DEV_UNREACHABLE &&
                   (int)PLINK_DEV_REFUSED == (int)UI_DEV_REFUSED,
               "DEV_ACK's results are the screen's");
_Static_assert(PLINK_DEV_MAX >= UI_DEVL_MAX, "the master's list fills the screen's");

/* The screen's copy of the mirror, made again when the mirror moves (a list, a record, an action,
 * the freshness): every row this firmware can use, in the master's order, through the one
 * converter of both screens (ui_dev_from_parts). While the device data isn't fresh, each tile
 * reads offline since the last device data (LINK-046). The mirror's state beside it follows every
 * turn: device data heard while it stays fresh (a summary every 10 s) moves heard_ms and nothing
 * the screen shows, and the console counts "last N s ago" from it. */
static void home_copy(uint64_t now)
{
    if (!s_home) {
        return;
    }
    plink_dev_info_t di;
    plink_keypad_dev_info(K.k, now, &di);
    if (K.dev_copied && di.seq == K.dev_seq_copied && di.fresh == K.dev_fresh_copied) {
        /* the rows stand; s_kdev_info is only this task's to write, so read here unlocked */
        if (di.heard_ms != s_kdev_info.heard_ms || di.shared != s_kdev_info.shared) {
            xSemaphoreTake(s_dev_lock, portMAX_DELAY);
            s_kdev_info = di;
            xSemaphoreGive(s_dev_lock);
        }
        return;
    }
    const int64_t epoch = link_epoch_ms();
    const int64_t heard = di.heard_ms != 0u && epoch != 0 && now >= di.heard_ms
                              ? epoch - (int64_t)(now - di.heard_ms)
                              : 0; /* 0: not known, the tile says "Offline" */
    xSemaphoreTake(s_dev_lock, portMAX_DELAY);
    size_t n = 0u;
    for (unsigned i = 0; i < di.count && n < PLINK_DEV_MAX; i++) {
        plink_dev_view_t v;
        if (!plink_keypad_dev_view(K.k, now, (uint8_t)i, &v) || !v.usable) {
            continue; /* a row this firmware can't use: no tile to tap */
        }
        const ui_dev_parts_t p = {.kind = v.cat.kind,
                                  .source = v.cat.source,
                                  .online = v.online,
                                  .pending = v.pending,
                                  .failed = v.failed,
                                  .unsure = v.unsure,
                                  .action = v.action,
                                  .ref = v.cat.ref,
                                  .name = v.cat.name,
                                  .area = v.cat.area,
                                  .state = v.word,
                                  .values = &v.values};
        ui_dev_from_parts(&s_kdevs[n], &p);
        if (v.since_last_data) {
            s_kdevs[n].since_ms = heard;
        }
        n++;
    }
    s_kdev_n = n;
    s_kdev_fresh = di.fresh;
    s_kdev_info = di;
    s_kdev_at = now;
    xSemaphoreGive(s_dev_lock);
    K.dev_copied = true;
    K.dev_seq_copied = di.seq;
    K.dev_fresh_copied = di.fresh;
}

size_t ksvc_dev_copy(ui_dev_t *out, size_t max, ksvc_dev_info_t *info)
{
    if (info != NULL) {
        memset(info, 0, sizeof *info);
        info->offered = s_home;
    }
    if (!s_home) {
        return 0u;
    }
    xSemaphoreTake(s_dev_lock, portMAX_DELAY);
    const size_t n = out == NULL ? 0u : s_kdev_n < max ? s_kdev_n : max;
    if (n > 0u) {
        memcpy(out, s_kdevs, n * sizeof *out);
    }
    if (info != NULL) {
        info->fresh = s_kdev_fresh;
        info->rows = s_kdev_n;
        info->mirror = s_kdev_info;
        info->copied_ms = s_kdev_at;
    }
    xSemaphoreGive(s_dev_lock);
    return n;
}

/* What the screen's copy says of the device named ref (*found false: no such row), and whether
 * the device data is fresh. */
static bool dev_seen(const char *ref, ui_dev_t *out, bool *found)
{
    *found = false;
    xSemaphoreTake(s_dev_lock, portMAX_DELAY);
    const bool fresh = s_kdev_fresh;
    for (size_t i = 0; i < s_kdev_n && !*found; i++) {
        if (strcmp(s_kdevs[i].ref, ref) == 0) {
            *out = s_kdevs[i];
            *found = true;
        }
    }
    xSemaphoreGive(s_dev_lock);
    return fresh;
}

/* The screen's wait for the answer to gen, ms at most (a late answer to one given up on can wake
 * it: the wait goes on for the time left). */
static bool act_wait(uint32_t gen, uint32_t ms, dev_ans_t *out)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t wait = pdMS_TO_TICKS(ms);
    for (TickType_t gone = 0; gone < wait; gone = xTaskGetTickCount() - start) {
        if (xSemaphoreTake(s_act_done, wait - gone) != pdTRUE) {
            return false;
        }
        bool mine = false;
        portENTER_CRITICAL(&s_act_mux);
        if (s_aans.gen == gen) {
            *out = s_aans;
            mine = true;
        }
        portEXIT_CRITICAL(&s_act_mux);
        if (mine) {
            return true;
        }
    }
    return false;
}

ui_dev_result_t ksvc_dev_act(const char *ref, uint8_t action, int32_t value, const char *code,
                             alarm_result_t *code_result)
{
    if (!s_home || K.k == NULL || K.setup_only || ref == NULL) {
        return UI_DEV_UNREACHABLE;
    }
    const bool coded = code != NULL && code[0] != '\0';
    /* 1-3: what the screen's copy already tells, answered at once with nothing sent (the master
     * judges it all again) */
    ui_dev_t d;
    memset(&d, 0, sizeof d);
    bool found = false;
    if (!dev_seen(ref, &d, &found)) {
        return UI_DEV_UNREACHABLE; /* not ONLINE, or the device data is stale (LINK-046) */
    }
    if (!found) {
        return UI_DEV_REFUSED;
    }
    if (d.status == UI_DEV_OFFLINE) {
        return UI_DEV_UNREACHABLE;
    }
    if (d.source == (uint8_t)DSRC_ZWAVE ||
        !hc_action_valid((dev_kind_t)d.kind, (dev_action_t)action)) {
        return UI_DEV_REFUSED; /* a Z-Wave row until BL-169 */
    }
    if (!coded && hc_needs_code((dev_kind_t)d.kind, (dev_action_t)action, value)) {
        return UI_DEV_NEED_CODE; /* the pad opens with no round trip */
    }
    /* 4: a thermostat's mode as its DEV_MODE_* bit */
    const uint8_t mode = action == (uint8_t)ACT_SET_MODE ? ui_dev_mode_bit(value) : 0u;
    const size_t ref_len = strlen(ref);
    const size_t code_len = coded ? strlen(code) : 0u;
    if ((action == (uint8_t)ACT_SET_MODE && mode == 0u) || ref_len > PLINK_REF_MAX ||
        code_len > ALARM_CODE_MAX_LEN) {
        return UI_DEV_REFUSED; /* never cut: a cut ref could name another device */
    }
    /* 5: to the link task, which sends it (and resends it for 3 s) */
    xSemaphoreTake(s_act_mutex, portMAX_DELAY);
    (void)xSemaphoreTake(s_act_done, 0); /* the answer to one given up on */
    portENTER_CRITICAL(&s_act_mux);
    memset(&s_abox.ask, 0, sizeof s_abox.ask);
    memcpy(s_abox.ask.ref, ref, ref_len);
    s_abox.ask.action = action;
    s_abox.ask.value = value;
    s_abox.ask.mode = mode;
    memcpy(s_abox.ask.code, code_len > 0u ? code : "", code_len);
    if (++s_act_gen == 0u) {
        s_act_gen = 1u;
    }
    const uint32_t gen = s_act_gen;
    s_abox.gen = gen;
    s_abox.pending = true;
    portEXIT_CRITICAL(&s_act_mux);
    dev_ans_t a;
    memset(&a, 0, sizeof a);
    uint8_t r = PLINK_DEV_OK;
    if (act_wait(gen, coded ? DEV_WAIT_CODE_MS : DEV_WAIT_MS, &a)) {
        r = a.result;
    } else {
        bool withdrawn = false;
        portENTER_CRITICAL(&s_act_mux);
        if (s_abox.pending && s_abox.gen == gen) {
            s_abox.pending = false;
            plink_wipe(&s_abox.ask, sizeof s_abox.ask);
            withdrawn = true;
        }
        portEXIT_CRITICAL(&s_act_mux);
        if (withdrawn) {
            r = PLINK_DEV_UNREACHABLE; /* the link task never took it: nothing was sent */
        } else {
            /* 7: no answer in time. The link task shows it pending from now on (until the answer,
             * a late refusal, or the give-up's "Not confirmed"); its word comes within a turn,
             * so the screen's next look already shows it. An answer that came meanwhile counts. */
            atomic_store(&s_act_abandon, gen);
            r = act_wait(gen, DEV_RELEASE_MS, &a) && !a.released ? a.result : PLINK_DEV_OK;
        }
    }
    xSemaphoreGive(s_act_mutex);
    /* 6: the answer as the screen reads it (UNAVAILABLE: the master serves no devices now) */
    if (r == PLINK_DEV_CODE_REFUSED && code_result != NULL) {
        *code_result = (alarm_result_t)a.why; /* the pad says why, lockout included */
    }
    return r <= PLINK_DEV_REFUSED ? (ui_dev_result_t)r : UI_DEV_UNREACHABLE;
}

/* The link task's side of the answer (after the screen's copy shows what the action did). */
static void act_answer(uint32_t gen, uint8_t result, uint8_t why, bool released)
{
    portENTER_CRITICAL(&s_act_mux);
    s_aans.gen = gen;
    s_aans.result = result;
    s_aans.why = why;
    s_aans.released = released;
    portEXIT_CRITICAL(&s_act_mux);
    xSemaphoreGive(s_act_done);
}

static void act_answer_later(uint32_t gen, uint8_t result, uint8_t why, bool released)
{
    K.act_due = true;
    K.act_due_gen = gen;
    K.act_res = result;
    K.act_why = why;
    K.act_released = released;
}

static void take_dev_act(uint64_t now)
{
    if (!s_home || K.act_active) {
        return;
    }
    dev_ask_t ask;
    uint32_t gen = 0;
    bool got = false;
    portENTER_CRITICAL(&s_act_mux);
    if (s_abox.pending) {
        ask = s_abox.ask;
        gen = s_abox.gen;
        s_abox.pending = false;
        plink_wipe(&s_abox.ask, sizeof s_abox.ask);
        got = true;
    }
    portEXIT_CRITICAL(&s_act_mux);
    if (!got) {
        return;
    }
    uint32_t ticket = 0u;
    const uint8_t r = plink_keypad_dev_act(K.k, now, ask.ref, ask.action, ask.value, ask.mode,
                                           ask.code[0] != '\0' ? ask.code : NULL, &ticket);
    plink_wipe(&ask, sizeof ask); /* the manager wipes its own after the last copy (LINK-018) */
    if (r != PLINK_DEV_OK) {
        act_answer(gen, r, 0u, false); /* refused at once: nothing sent */
        return;
    }
    K.act_active = true;
    K.act_gen = gen;
    K.act_ticket = ticket;
}

static void track_dev_act(void)
{
    if (!K.act_active) {
        return;
    }
    /* The master's answer first, even when the screen's wait has just ended: one taken in this
     * very turn (ksvc_input runs before ksvc_poll) reaches the screen's short grace wait
     * (ksvc_dev_act), so the pad says "wrong code" or the lockout rather than closing as if the
     * action went. Its record is in the mirror. */
    plink_dev_answer_t ans;
    const plink_cmd_state_t st = plink_keypad_dev_act_state(K.k, K.act_ticket, &ans);
    if (st == PLINK_CMD_DONE) {
        act_answer_later(K.act_gen, ans.result, ans.why, false);
        K.act_active = false;
        return;
    }
    if (st == PLINK_CMD_PENDING && atomic_load(&s_act_abandon) != K.act_gen) {
        return;
    }
    /* Still being sent once the screen stopped waiting, or given up (3 s) while it still waited
     * (its 3 s with a code): its tile shows it pending - failed for a late refusal, failed and
     * unsure once given up ("Not confirmed") - until a later record (docs/features/11 "Device
     * actions" 7). */
    plink_keypad_dev_act_release(K.k, K.act_ticket);
    act_answer_later(K.act_gen, PLINK_DEV_OK, 0u, true);
    K.act_active = false;
}

/* After home_copy: the screen hears the answer once its copy shows what the action did. */
static void dev_answer_flush(void)
{
    if (K.act_due) {
        K.act_due = false;
        act_answer(K.act_due_gen, K.act_res, K.act_why, K.act_released);
    }
}

/* The mirror and the screen's copy (start-up): all or nothing. Never on a board without PSRAM. */
static void home_prepare(void)
{
#if CONFIG_ALARM_HOME_CONTROL
    if (s_home || !alarm_mem_has_psram()) {
        return;
    }
    s_mirror = alarm_mem_slow_calloc(1, sizeof *s_mirror);
    s_kdevs = alarm_mem_slow_calloc(PLINK_DEV_MAX, sizeof *s_kdevs);
    /* PSRAM, as the buffers: internal RAM is the alarm's reserve */
    s_dev_lock = xSemaphoreCreateMutexWithCaps(alarm_mem_slow_caps());
    s_act_mutex = xSemaphoreCreateMutexWithCaps(alarm_mem_slow_caps());
    s_act_done = xSemaphoreCreateBinaryWithCaps(alarm_mem_slow_caps());
    s_home = s_mirror != NULL && s_kdevs != NULL && s_dev_lock != NULL && s_act_mutex != NULL &&
             s_act_done != NULL;
    if (s_home) {
        return;
    }
    ESP_LOGE(TAG, "no room for Smart Home on this keypad: the link runs without it");
    free(s_mirror);
    free(s_kdevs);
    s_mirror = NULL;
    s_kdevs = NULL;
    if (s_dev_lock != NULL) {
        vSemaphoreDeleteWithCaps(s_dev_lock);
        s_dev_lock = NULL;
    }
    if (s_act_mutex != NULL) {
        vSemaphoreDeleteWithCaps(s_act_mutex);
        s_act_mutex = NULL;
    }
    if (s_act_done != NULL) {
        vSemaphoreDeleteWithCaps(s_act_done);
        s_act_done = NULL;
    }
#endif
}

/* ---- settings: this keypad's Screen and sound for its master (LINK-050..052, UI-046) --------- */

/* This keypad's voice in the link's numbers; *present: this build has one. */
static uint8_t keypad_voice(bool *present)
{
    panel_voice_status_t v;
    panel_voice_status(&v);
    *present = v.state != PANEL_VOICE_OFF;
    switch (v.state) {
    case PANEL_VOICE_READY: return PLINK_VOICE_READY;
    case PANEL_VOICE_LOADING: return PLINK_VOICE_LOADING;
    default: return PLINK_VOICE_NONE; /* none in this build, no pack, or one it can't use */
    }
}

/* The fields this keypad takes: every one its screen takes (UI-046), the announcements' only
 * where its screen shows them (a build with the voice), as the master's own (ha_link). */
static uint32_t keypad_accepts(bool voice_present)
{
    const proto_prefs_mask_t voice =
        PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_VOLUME) | PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_ENABLED) |
        PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_STATUS) | PROTO_PREFS_BIT(PROTO_PREFS_F_VOICE_CHIME);
    return voice_present ? PROTO_PREFS_KNOWN : (PROTO_PREFS_KNOWN & ~voice);
}

static void voice_look(uint64_t now)
{
    bool present = false;
    K.voice = keypad_voice(&present);
    K.accepts = keypad_accepts(present);
    K.voice_at = now + PREFS_VOICE_MS;
}

/* The values stored now (panel_prefs_current: the RAM copy, never NVS) as a fields body of the
 * fields it takes: its length, 0 before the start-up's first load (nothing to report yet). */
static size_t prefs_body(uint8_t *out, size_t cap)
{
    panel_prefs_t p;
    if (!panel_prefs_current(&p)) {
        return 0u;
    }
    proto_prefs_values_t v;
    panel_prefs_to_values(&p, &v);
    return proto_prefs_encode(&v, K.accepts, out, cap);
}

/* The report (LINK-050): after every save (panel_prefs_seq moves, whoever saved), or when the
 * voice changes; the manager sends it at a session's first STATUS, then at most once a second. */
static void prefs_report(uint64_t now)
{
    if (!K.prefs_on) {
        return;
    }
    if (now >= K.voice_at) {
        voice_look(now);
    }
    const uint32_t seq = panel_prefs_seq();
    if (K.prefs_reported && seq == K.prefs_seq && K.voice == K.prefs_voice &&
        K.accepts == K.prefs_accepts) {
        return;
    }
    uint8_t body[PROTO_PREFS_ENC_MAX];
    const size_t len = prefs_body(body, sizeof body);
    if (len == 0u) {
        return;
    }
    if (!plink_keypad_prefs_report(K.k, now, g_link.panel_id, K.voice, K.accepts, body, len)) {
        ESP_LOGE(TAG, "this keypad's settings couldn't be reported");
    }
    K.prefs_reported = true;
    K.prefs_seq = seq;
    K.prefs_voice = K.voice;
    K.prefs_accepts = K.accepts;
}

static void pset_refuse(uint8_t id, uint8_t why)
{
    if (K.pset_n < PLINK_PREFS_REFUSED_MAX) {
        s_pset_refused[2u * K.pset_n] = id;
        s_pset_refused[2u * K.pset_n + 1u] = why;
        K.pset_n++;
    }
}

static void pset_refuse_all(uint32_t have, uint8_t why)
{
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        if ((have & PROTO_PREFS_BIT(id)) != 0u) {
            pset_refuse((uint8_t)id, why);
        }
    }
}

/* The answer: the fields refused and the values stored now (whatever the screen applied). */
static void pset_answer(uint64_t now)
{
    uint8_t body[PROTO_PREFS_ENC_MAX];
    const size_t len = prefs_body(body, sizeof body);
    (void)plink_keypad_prefs_answer(K.k, now, K.pset_req, s_pset_refused, K.pset_n,
                                    len > 0u ? body : NULL, len);
    K.pset_active = false;
    K.pset_ticket = 0u;
    K.pset_have = 0u;
    K.pset_n = 0u;
}

/* The screen's answer (panel_prefs_done_fn): on its task, inside its tick - only a copy. */
static void pset_done(void *ctx, uint32_t ticket, const uint8_t why[PROTO_PREFS_F_LAST])
{
    (void)ctx;
    portENTER_CRITICAL(&s_pset_mux);
    s_pset_ans.set = true;
    s_pset_ans.ticket = ticket;
    memcpy(s_pset_ans.why, why, sizeof s_pset_ans.why);
    portEXIT_CRITICAL(&s_pset_mux);
}

static const char *origin_word(uint8_t origin)
{
    switch (origin) {
    case PLINK_ORIGIN_HA: return "Home Assistant";
    case PLINK_ORIGIN_CONSOLE: return "its console";
    default: return "elsewhere";
    }
}

/* A change from the master (PREFS_SET, LINK-051): each field checked here - a field this keypad
 * doesn't take, out of range, named twice or of the wrong length is refused here - and the rest
 * handed to the screen (panel_prefs' mailbox), which applies them through its own save or refuses
 * them (IN_USE, HELD, VOICE_NOT_READY). One at a time; the manager keeps the others waiting. */
static void pset_take(uint64_t now)
{
    if (!K.prefs_on || K.pset_active) {
        return;
    }
    plink_prefs_change_t c;
    if (!plink_keypad_take_prefs_set(K.k, &c)) {
        return;
    }
    K.pset_req = c.req;
    K.pset_n = 0u;
    K.pset_have = 0u;
    proto_prefs_decoded_t d;
    if (!proto_prefs_decode(c.fields, c.len, K.accepts, &d)) {
        ESP_LOGW(TAG, "a settings change that isn't a fields list: nothing applied");
        pset_answer(now); /* (the link's codec lets none through) */
        return;
    }
    for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
        if ((d.refused & PROTO_PREFS_BIT(id)) != 0u) {
            pset_refuse((uint8_t)id, d.why[id - 1u]);
        }
    }
    for (size_t i = 0; i < d.unknown_n && i < PROTO_PREFS_UNKNOWN_MAX; i++) {
        pset_refuse(d.unknown[i], PROTO_PREFS_WHY_NOT_ALLOWED); /* a newer master's field */
    }
    ESP_LOGI(TAG, "settings change from %s through the alarm panel: %u field(s), %u refused here",
             origin_word(c.origin), (unsigned)__builtin_popcount(d.values.have),
             (unsigned)K.pset_n);
    if (d.values.have == 0u) {
        pset_answer(now);
        return;
    }
    portENTER_CRITICAL(&s_pset_mux);
    s_pset_ans.set = false;
    portEXIT_CRITICAL(&s_pset_mux);
    const uint32_t ticket = panel_prefs_remote_submit(&d.values, pset_done, NULL);
    if (ticket == 0u) {
        pset_refuse_all(d.values.have, PROTO_PREFS_WHY_BUSY); /* another change is under way */
        pset_answer(now);
        return;
    }
    K.pset_active = true;
    K.pset_cancel_tried = false;
    K.pset_ticket = ticket;
    K.pset_have = d.values.have;
    K.pset_since = now;
}

static void pset_track(uint64_t now)
{
    if (!K.pset_active) {
        return;
    }
    uint8_t why[PROTO_PREFS_F_LAST];
    bool got = false;
    portENTER_CRITICAL(&s_pset_mux);
    if (s_pset_ans.set && s_pset_ans.ticket == K.pset_ticket) {
        memcpy(why, s_pset_ans.why, sizeof why);
        s_pset_ans.set = false;
        got = true;
    }
    portEXIT_CRITICAL(&s_pset_mux);
    if (got) {
        for (unsigned id = 1u; id <= PROTO_PREFS_F_LAST; id++) {
            if ((K.pset_have & PROTO_PREFS_BIT(id)) != 0u && why[id - 1u] != 0u) {
                pset_refuse((uint8_t)id, why[id - 1u]);
            }
        }
        pset_answer(now);
        return;
    }
    if (now - K.pset_since >= UI_PREFS_TAKE_MS && !K.pset_cancel_tried) {
        /* Not taken in 3 s (the screen is stuck): withdrawn, every field BUSY. Taken, its answer
         * comes within 2 s (UI_PREFS_LOOK_MS). */
        K.pset_cancel_tried = true;
        if (panel_prefs_remote_cancel(K.pset_ticket)) {
            pset_refuse_all(K.pset_have, PROTO_PREFS_WHY_BUSY);
            pset_answer(now);
            return;
        }
    }
    if (now - K.pset_since >= PSET_LOST_MS) {
        ESP_LOGE(TAG, "the screen took a settings change and never answered: answered busy");
        pset_refuse_all(K.pset_have, PROTO_PREFS_WHY_BUSY);
        pset_answer(now);
    }
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
        if (K.voiced && plink_vfeed_live(&K.vf)) {
            /* tracked, so words that follow the tone wait for its end (D04), then handed on */
            const uint32_t ticket = sounder_play_tracked((alarm_sound_t)s.sound);
            plink_vfeed_sound(&K.vf, now, &s, ticket);
        } else {
            sounder_play((alarm_sound_t)s.sound); /* no ticket nobody would read */
        }
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

/* ---- the voice (LINK-028) -------------------------------------------------------------------- */

static void vf_baseline(void *ctx, uint8_t partition, const alarm_snapshot_t *snap)
{
    (void)ctx;
    s_voice->baseline(partition, snap);
}

static void vf_pause(void *ctx)
{
    (void)ctx;
    s_voice->pause();
}

static void vf_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    s_voice->event(ev);
}

static void vf_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone,
                     uint32_t ticket)
{
    (void)ctx;
    s_voice->sound(partition, sound, zone, ticket);
}

static void vf_output(void *ctx, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    (void)ctx;
    s_voice->output(output, partition, pattern);
}

static void vf_snapshot(void *ctx, const alarm_snapshot_t *snap)
{
    (void)ctx;
    s_voice->snapshot(snap);
}

void plink_service_set_voice(const plink_service_voice_t *v)
{
    const bool whole = v != NULL && v->baseline != NULL && v->pause != NULL && v->event != NULL &&
                       v->sound != NULL && v->output != NULL && v->snapshot != NULL;
    s_voice = whole ? v : NULL;
}

void plink_service_voice_resync(void)
{
    atomic_store(&s_voice_resync, true);
}

/* After sounds() and copy_for_ui() (the configuration copy the voice reads is the one the
 * snapshot's config_seq names): what the keypad holds goes to plink_vfeed, which pauses,
 * baselines, feeds the covered events and commits. The events are read only while ONLINE. */
static void voice_feed(uint64_t now, const plink_keypad_info_t *info)
{
    if (!K.voiced) {
        return;
    }
    const bool online = info->state == PLINK_LINK_ONLINE;
    const bool resync = atomic_exchange(&s_voice_resync, false); /* offline: ONLINE gives one */
    size_t n = 0u;
    if (online) {
        n = plink_keypad_events(K.k, s_ev_tmp, PLINK_EVENTS_KEPT);
    }
    const plink_vfeed_view_t v = {.online = online,
                                  .home = info->home,
                                  .snap = plink_keypad_snapshot(K.k),
                                  .ev = s_ev_tmp,
                                  .n_ev = n,
                                  .epoch_ms = plink_keypad_epoch_ms(K.k, now),
                                  .resync = resync,
                                  .mono_ms = plink_keypad_mono_ms(K.k, now)};
    plink_vfeed_poll(&K.vf, now, &v);
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

/* needs_update (ui_link_info_t): the configuration the master's latest status names is one this
 * keypad couldn't read. The manager forgets a failed one once another installs or the master
 * names the installed one again, so a link lost later says only that. */
static bool needs_update(const plink_keypad_info_t *info)
{
    return info->cfg_bad_crc != 0u;
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
        s_ui_cfg_seq = cfg_seq;
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

bool plink_service_keypad_config(alarm_config_t *out, uint32_t *config_seq)
{
    if (s_ui_cfg == NULL || out == NULL) {
        return false;
    }
    link_pub_lock();
    const bool have = s_ui_have_cfg && !g_pub.setup_keypad; /* as ksvc_ui_config */
    if (have) {
        memcpy(out, s_ui_cfg, sizeof *out);
        if (config_seq != NULL) {
            *config_seq = s_ui_cfg_seq;
        }
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
    if (K.k == NULL || setup_resting()) {
        return UINT64_MAX;
    }
    const uint64_t k = plink_keypad_next(K.k);
    const uint64_t v = K.voiced ? plink_vfeed_next(&K.vf) : UINT64_MAX; /* a hold or settle ends */
    return k < v ? k : v;
}

void ksvc_poll(uint64_t now)
{
    if (K.k == NULL) {
        return;
    }
    if (!K.setup_only) {
        take_command(now);
        take_dev_act(now); /* its first copy goes now, after the alarm's command */
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
        track_dev_act();
        pset_track(now);
        pset_take(now);
        prefs_report(now);
        network(now);
        sounds(now, &info);
        follow_time(now);
        beat(now, &info);
        maintenance_mark(now, &info);
    }
    copy_for_ui(now);
    home_copy(now);
    dev_answer_flush();     /* after the copy: the screen's next look shows what its action did */
    voice_feed(now, &info); /* after the copy: the words its snapshot's config_seq names */
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
    s_ui_cfg_seq = 0u;
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
    if (!setup_only && s_voice != NULL) {
        /* LINK-028: the keypad's voice hears its master from the first ONLINE on */
        static const plink_vfeed_port_t vport = {.ctx = NULL,
                                                 .baseline = vf_baseline,
                                                 .pause = vf_pause,
                                                 .event = vf_event,
                                                 .sound = vf_sound,
                                                 .output = vf_output,
                                                 .snapshot = vf_snapshot};
        plink_vfeed_init(&K.vf, &vport);
        K.voiced = true;
    }
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
    /* What it offers (LINK-043), each with the code that uses it: Smart Home with its mirror
     * (attached below; the core offers HOME only then), its settings with a screen to apply them
     * (UI-046). The setup's pairing on a master offers nothing: that panel isn't a keypad yet. */
    uint16_t offer = 0u;
    if (!setup_only) {
        home_prepare();
#if CONFIG_ALARM_FEATURE_UI
        offer |= PLINK_CAP_PREFS;
#endif
        offer |= s_home ? PLINK_CAP_HOME : 0u;
    }
    plink_keypad_init(k, &g_link.crypto, &port, have ? &rec : NULL, K.cfg_buf,
                      ALARM_CONFIG_BLOB_MAX, K.cfg, PLINK_KIND_PANEL, &caps, offer, name, g_link.fw,
                      now);
    plink_wipe(&rec, sizeof rec);
    if ((offer & PLINK_CAP_HOME) != 0u) {
        plink_keypad_home_attach(k, s_mirror); /* before the first tick */
    }
    K.prefs_on = (offer & PLINK_CAP_PREFS) != 0u;
    if (K.prefs_on) {
        voice_look(now); /* what it takes, before a change can come */
    }
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
