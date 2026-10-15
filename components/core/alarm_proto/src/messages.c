/*
 * messages.c - topics and the JSON the panel publishes: partition status, zone state,
 * troubles, events and the heartbeat (see proto.h and docs/features/10).
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_strings.h"
#include "notify/notify_auto.h"
#include "alarm_proto/proto.h"
#include "alarm_proto/proto_zb.h"

/* ---- identity and topics ----------------------------------------------------------------- */

static bool word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_';
}

bool proto_prefix_valid(const char *prefix)
{
    if (prefix == NULL) {
        return false;
    }
    const size_t n = strlen(prefix);
    if (n == 0u || n >= PROTO_PREFIX_LEN || prefix[0] == '/' || prefix[n - 1u] == '/') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (prefix[i] == '/') {
            if (prefix[i + 1u] == '/') {
                return false;
            }
        } else if (!word_char(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool proto_panel_id_valid(const char *panel_id)
{
    if (panel_id == NULL) {
        return false;
    }
    const size_t n = strlen(panel_id);
    if (n == 0u || n >= PROTO_PANEL_ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = panel_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

void proto_panel_id_from_mac(const uint8_t mac[6], char out[PROTO_PANEL_ID_LEN])
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 3u; i++) {
        out[i * 2u] = hex[mac[3u + i] >> 4];
        out[i * 2u + 1u] = hex[mac[3u + i] & 15u];
    }
    out[6] = '\0';
}

static bool fits(int n, size_t cap)
{
    return n > 0 && (size_t)n < cap;
}

bool proto_topic(const proto_ident_t *id, const char *suffix, char *out, size_t cap)
{
    return fits(snprintf(out, cap, "%s/%s%s", id->prefix, id->panel_id, suffix), cap);
}

bool proto_topic_partition(const proto_ident_t *id, uint8_t partition, const char *leaf,
                           char *out, size_t cap)
{
    return fits(snprintf(out, cap, "%s/%s/partition/%u/%s", id->prefix, id->panel_id,
                         (unsigned)partition, leaf),
                cap);
}

bool proto_topic_zone(const proto_ident_t *id, uint16_t zone, char *out, size_t cap)
{
    return fits(snprintf(out, cap, "%s/%s/zone/%u/state", id->prefix, id->panel_id,
                         (unsigned)zone),
                cap);
}

/* ---- helpers ----------------------------------------------------------------------------- */

const char *proto_ha_state(const alarm_partition_status_t *ps)
{
    if (ps->triggered) {
        return "triggered";
    }
    switch (ps->state) {
    case ALARM_STATE_EXIT_DELAY: return "arming";
    case ALARM_STATE_ENTRY_DELAY: return "pending";
    case ALARM_STATE_ARMED:
        switch (ps->level) {
        case ALARM_LEVEL_STAY: return "armed_home";
        case ALARM_LEVEL_NIGHT: return "armed_night";
        default: return "armed_away";
        }
    default: return "disarmed";
    }
}

/* Days since 1970-01-01 to a civil date (H. Hinnant's algorithm). */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10u ? mp + 3u : mp - 9u;
    *y = (int)((int64_t)yoe + era * 400 + (*m <= 2u ? 1 : 0));
}

bool proto_iso8601(int64_t epoch_ms, char out[24])
{
    out[0] = '\0';
    if (epoch_ms <= 0 || epoch_ms >= 253402300800000LL) { /* before 1970 or after 9999 */
        return false;
    }
    const int64_t secs = epoch_ms / 1000;
    const unsigned rem = (unsigned)(secs % 86400);
    int y;
    unsigned m;
    unsigned d;
    civil_from_days(secs / 86400, &y, &m, &d);
    return fits(snprintf(out, 24, "%04d-%02u-%02uT%02u:%02u:%02uZ", y, m, d, rem / 3600u,
                         rem / 60u % 60u, rem % 60u),
                24);
}

static const char *user_name(const alarm_config_t *cfg, uint8_t user)
{
    if (user == 0u || user > ALARM_MAX_USERS || cfg->users[user - 1u].name[0] == '\0') {
        return NULL;
    }
    return cfg->users[user - 1u].name;
}

/* An automation's words for the JSON (docs/features/41 D30, AUT-009): its whole sentence, else
 * its trigger clause, in English; NULL when the hook can't say (none, uid 0, or deleted). */
static const char *auto_words(const notify_auto_info_t *info, notify_auto_found_t found)
{
    if (found != NOTIFY_AUTO_FOUND) {
        return NULL;
    }
    if (info->sentence[0] != '\0') {
        return info->sentence;
    }
    return (info->clause[0] != '\0') ? info->clause : NULL;
}

static const char *zone_name(const alarm_config_t *cfg, uint16_t zone)
{
    if (zone == 0u || zone > ALARM_MAX_ZONES || cfg->zones[zone - 1u].name[0] == '\0') {
        return NULL;
    }
    return cfg->zones[zone - 1u].name;
}

static const char *device_name(const alarm_config_t *cfg, uint16_t device)
{
    if (device == 0u || device > ALARM_MAX_DEVICES || cfg->devices[device - 1u].name[0] == '\0') {
        return NULL;
    }
    return cfg->devices[device - 1u].name;
}

static void levels_array(pj_writer_t *w, const char *key, uint8_t mask)
{
    static const alarm_arm_level_t levels[] = {ALARM_LEVEL_AWAY, ALARM_LEVEL_STAY,
                                               ALARM_LEVEL_NIGHT};
    pj_karr(w, key);
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if ((mask & ALARM_LEVEL_BIT(levels[i])) != 0u) {
            pj_str(w, alarm_level_name(levels[i]));
        }
    }
    pj_arr_end(w);
}

/* ---- partition status -------------------------------------------------------------------- */

void proto_partition_status(pj_writer_t *w, const alarm_config_t *cfg,
                            const alarm_snapshot_t *snap, uint8_t partition,
                            const proto_when_t *when)
{
    if (partition == 0u || partition > ALARM_MAX_PARTITIONS) {
        w->overflow = true;
        return;
    }
    const alarm_partition_status_t *ps = &snap->part[partition - 1u];
    pj_obj(w);
    pj_kuint(w, "partition", partition);
    pj_kstr(w, "name", cfg->partitions[partition - 1u].name);
    pj_kstr(w, "state", alarm_state_name(ps->state));
    pj_kstr(w, "ha_state", proto_ha_state(ps));
    pj_kstr(w, "level", ps->level == ALARM_LEVEL_NONE ? NULL : alarm_level_name(ps->level));
    pj_kbool(w, "no_entry_delay", (ps->arm_flags & ALARM_ARM_NO_ENTRY_DELAY) != 0u);
    pj_kbool(w, "silent_exit", (ps->arm_flags & ALARM_ARM_SILENT_EXIT) != 0u);
    pj_kbool(w, "ready", ps->ready);
    levels_array(w, "ready_levels", ps->ready_levels);
    if (when != NULL) {
        const alarm_ms_t gone = (when->now > snap->taken_ms) ? when->now - snap->taken_ms : 0u;
        const alarm_ms_t left = (ps->delay_remaining_ms > gone) ? ps->delay_remaining_ms - gone
                                                                : 0u;
        pj_kuint(w, "delay_remaining_s", (left + 999u) / 1000u);
    }
    pj_kbool(w, "triggered", ps->triggered);
    pj_kbool(w, "exit_error", ps->exit_error);
    pj_kbool(w, "chime", ps->chime);
    pj_kbool(w, "walk_test", ps->walk_test);
    pj_kuint(w, "walk_test_tested", ps->walk_test ? ps->walk_tested : 0u);
    pj_kuint(w, "walk_test_total", ps->walk_test ? ps->walk_total : 0u);
    if (when != NULL) {
        /* Counted down like delay_remaining_s; 0 when no test runs (WALK-019). */
        const alarm_ms_t gone = (when->now > snap->taken_ms) ? when->now - snap->taken_ms : 0u;
        const alarm_ms_t left = (ps->walk_test && ps->walk_test_remaining_ms > gone)
                                    ? ps->walk_test_remaining_ms - gone
                                    : 0u;
        pj_kuint(w, "walk_test_remaining_s", (left + 999u) / 1000u);
    }
    pj_kbool(w, "installer_mode", snap->program);
    if (ps->armed_by != 0u) {
        pj_kobj(w, "armed_by");
        pj_kuint(w, "user", ps->armed_by);
        pj_kstr(w, "name", user_name(cfg, ps->armed_by));
        if (ps->arm_source.kind == (uint8_t)ALARM_SRC_AUTOMATION) {
            /* D30, AUT-009, MQTT-004: an automation armed it, as the user above (who saved it):
             * consumers read "source" first and never name that user as the one who armed.
             * After a restart the uid isn't known: no source_id, automation null. */
            notify_auto_info_t info;
            const notify_auto_found_t found =
                notify_auto_lookup(NULL, cfg, ps->arm_source.id, &info);
            pj_kstr(w, "source", alarm_source_name(ALARM_SRC_AUTOMATION));
            pj_knum(w, "source_id", ps->arm_source.id);
            pj_kstr(w, "automation", auto_words(&info, found));
        }
        pj_obj_end(w);
    } else {
        pj_knull(w, "armed_by");
    }
    pj_karr(w, "memory");
    for (unsigned c = ALARM_CAUSE_BURGLARY; c < ALARM_CAUSE_COUNT; c++) {
        if ((ps->memory & (1u << c)) != 0u) {
            pj_str(w, alarm_cause_name((alarm_cause_t)c));
        }
    }
    pj_arr_end(w);
    pj_kbool(w, "memory_canceled", (ps->memory & ALARM_MEM_CANCELED) != 0u);
    pj_kuint(w, "faults", ps->faults);
    pj_kuint(w, "bypassed", ps->bypassed);
    pj_kuint(w, "alarms", ps->alarms);
    pj_kuint(w, "troubles", ps->troubles);
    pj_kbool(w, "trouble_beeps", ps->trouble_unack);
    pj_kstr(w, "sounder", alarm_sound_name(ps->sounder));
    if (when != NULL) {
        char ts[24];
        pj_kstr(w, "ts", proto_iso8601(when->epoch_ms, ts) ? ts : NULL);
    }
    pj_obj_end(w);
}

/* ---- zones and troubles ------------------------------------------------------------------ */

void proto_zone_state(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap,
                      uint16_t zone)
{
    proto_zone_state_diag(w, cfg, snap, zone, NULL);
}

void proto_zone_state_diag(pj_writer_t *w, const alarm_config_t *cfg,
                           const alarm_snapshot_t *snap, uint16_t zone,
                           const proto_zone_diag_t *diag)
{
    if (zone == 0u || zone > ALARM_MAX_ZONES) {
        w->overflow = true;
        return;
    }
    const alarm_zone_config_t *zc = &cfg->zones[zone - 1u];
    const uint16_t f = snap->zone[zone - 1u].flags;
    pj_obj(w);
    pj_kuint(w, "zone", zone);
    pj_kstr(w, "name", zc->name);
    pj_kstr(w, "type", alarm_zone_type_name(zc->type));
    pj_kstr(w, "device_class", alarm_device_class_name((alarm_device_class_t)zc->device_class));
    pj_kuint(w, "partition", zc->partition);
    pj_kbool(w, "open", (f & ALARM_ZS_FAULT) != 0u);
    pj_kbool(w, "bypassed", (f & ALARM_ZS_BYPASSED) != 0u);
    pj_kbool(w, "alarm", (f & ALARM_ZS_ALARM) != 0u);
    pj_kbool(w, "trouble", (f & ALARM_ZS_TROUBLE) != 0u);
    pj_kbool(w, "tamper", (f & ALARM_ZS_TAMPER) != 0u);
    pj_kbool(w, "low_battery", (f & ALARM_ZS_LOW_BATTERY) != 0u);
    pj_kbool(w, "supervision", (f & ALARM_ZS_SUPERVISION) != 0u);
    pj_kbool(w, "not_ready", (f & ALARM_ZS_NOT_READY) != 0u);
    /* The device feeding it (MQTT-022): only what it said, after the keys every zone has. */
    if (diag != NULL) {
        if (diag->battery <= 100u) {
            pj_kuint(w, "battery", diag->battery);
        }
        const char *signal = proto_signal_name(diag->signal);
        if (signal != NULL) {
            pj_kstr(w, "signal", signal);
        }
    }
    pj_obj_end(w);
}

/* ---- the Zigbee network (proto_zb.h, MQTT-021) ------------------------------------------- */

const char *proto_zb_radio_name(uint8_t radio)
{
    static const char *const t[PROTO_ZB_RADIO_COUNT] = {
        [PROTO_ZB_RADIO_NOT_CONNECTED] = "not_connected",
        [PROTO_ZB_RADIO_STARTING] = "starting",
        [PROTO_ZB_RADIO_WORKING] = "working",
        [PROTO_ZB_RADIO_NOT_ANSWERING] = "not_answering",
        [PROTO_ZB_RADIO_PAUSED] = "paused",
        [PROTO_ZB_RADIO_SOCKET_ZWAVE] = "socket_zwave",
        [PROTO_ZB_RADIO_UNUSABLE] = "unusable",
    };
    return radio < PROTO_ZB_RADIO_COUNT ? t[radio] : "unknown";
}

const char *proto_signal_name(uint8_t signal)
{
    switch ((proto_signal_t)signal) {
    case PROTO_SIGNAL_GOOD: return "good";
    case PROTO_SIGNAL_FAIR: return "fair";
    case PROTO_SIGNAL_WEAK: return "weak";
    default: return NULL;
    }
}

void proto_zb_state(pj_writer_t *w, const proto_zb_state_t *s)
{
    pj_obj(w);
    pj_kstr(w, "radio", proto_zb_radio_name(s->radio));
    if (s->channel >= 11u && s->channel <= 26u) {
        pj_kuint(w, "channel", s->channel);
    } else {
        pj_knull(w, "channel");
    }
    pj_kuint(w, "devices", s->devices);
    pj_kuint(w, "not_heard", s->not_heard);
    pj_kbool(w, "jammed", s->jammed);
    pj_obj_end(w);
}

static void sys_trouble_array(pj_writer_t *w, const char *key, uint32_t mask)
{
    pj_karr(w, key);
    for (unsigned t = 0; t < ALARM_TRBL_COUNT; t++) {
        if ((mask & (1u << t)) != 0u) {
            pj_str(w, alarm_sys_trouble_name((alarm_sys_trouble_t)t));
        }
    }
    pj_arr_end(w);
}

void proto_troubles(pj_writer_t *w, const alarm_config_t *cfg, const alarm_snapshot_t *snap)
{
    unsigned zones = 0;
    unsigned devices = 0;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if (cfg->zones[z - 1u].type != ALARM_ZT_NOT_USED && snap->zone[z - 1u].trouble != 0u) {
            zones++;
        }
    }
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        if ((snap->dev_troubles & (1u << d)) != 0u) {
            devices++;
        }
    }
    unsigned sys = 0;
    for (unsigned t = 0; t < ALARM_TRBL_COUNT; t++) {
        if ((snap->sys_troubles & (1u << t)) != 0u) {
            sys++;
        }
    }
    bool beeping = false;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((snap->partitions & (1u << p)) != 0u && snap->part[p].trouble_unack) {
            beeping = true;
        }
    }

    pj_obj(w);
    pj_kbool(w, "active", sys + zones + devices != 0u);
    pj_kuint(w, "count", sys + zones + devices);
    pj_kbool(w, "trouble_beeps", beeping);
    sys_trouble_array(w, "system", snap->sys_troubles);
    sys_trouble_array(w, "system_restored", snap->sys_troubles_restored);
    pj_karr(w, "zones");
    unsigned listed = 0;
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES && listed < PROTO_TROUBLE_ZONES_MAX; z++) {
        const uint8_t mask = snap->zone[z - 1u].trouble;
        if (cfg->zones[z - 1u].type == ALARM_ZT_NOT_USED || mask == 0u) {
            continue;
        }
        listed++;
        pj_obj(w);
        pj_kuint(w, "zone", z);
        pj_kstr(w, "name", cfg->zones[z - 1u].name);
        pj_karr(w, "troubles");
        for (unsigned t = 0; t < ALARM_ZTRBL_COUNT; t++) {
            if ((mask & (1u << t)) != 0u) {
                pj_str(w, alarm_zone_trouble_name((alarm_zone_trouble_t)t));
            }
        }
        pj_arr_end(w);
        pj_obj_end(w);
    }
    pj_arr_end(w);
    pj_kuint(w, "zones_more", zones - listed);
    pj_karr(w, "devices");
    for (unsigned d = 0; d < ALARM_MAX_DEVICES; d++) {
        if ((snap->dev_troubles & (1u << d)) != 0u) {
            pj_obj(w);
            pj_kuint(w, "device", d + 1u);
            pj_kstr(w, "name", device_name(cfg, (uint16_t)(d + 1u)));
            pj_obj_end(w);
        }
    }
    pj_arr_end(w);
    pj_obj_end(w);
}

/* ---- events ------------------------------------------------------------------------------ */

static void trouble_fields(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev)
{
    switch (ev->arg2) {
    case ALARM_TRBL_FAMILY_SYSTEM:
        pj_kstr(w, "family", "system");
        pj_kstr(w, "trouble", alarm_sys_trouble_name((alarm_sys_trouble_t)ev->arg));
        break;
    case ALARM_TRBL_FAMILY_DEVICE:
        pj_kstr(w, "family", "device");
        pj_kstr(w, "trouble", "supervision");
        pj_knum(w, "device", ev->zone);
        pj_kstr(w, "device_name", device_name(cfg, ev->zone));
        break;
    default:
        pj_kstr(w, "family", "zone");
        pj_kstr(w, "trouble", alarm_zone_trouble_name((alarm_zone_trouble_t)ev->arg));
        break;
    }
}

/* The event's fields into an already-open object: the Home Assistant link merges its own
 * "t":"event" in front (proto_link_event); the MQTT payload below stays byte-identical. */
void proto_event_fields(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev,
                        bool replayed)
{
    const bool device = (ev->type == ALARM_EV_TROUBLE || ev->type == ALARM_EV_TROUBLE_RESTORE) &&
                        ev->arg2 == ALARM_TRBL_FAMILY_DEVICE;
    char ts[24];
    pj_kuint(w, "seq", ev->seq);
    pj_kstr(w, "ts", proto_iso8601(ev->epoch_ms, ts) ? ts : NULL);
    pj_kstr(w, "event_type", alarm_event_name(ev->type));
    pj_knum(w, "partition", ev->partition);
    pj_knum(w, "zone", device ? 0u : ev->zone);
    pj_kstr(w, "zone_name", device ? NULL : zone_name(cfg, ev->zone));
    pj_knum(w, "user", ev->user);
    pj_kstr(w, "user_name", user_name(cfg, ev->user));
    pj_kstr(w, "source", alarm_source_name((alarm_source_kind_t)ev->source.kind));
    pj_kuint(w, "source_id", ev->source.id);

    switch (ev->type) {
    case ALARM_EV_ARMED:
    case ALARM_EV_AUTO_STAY:
        pj_kstr(w, "level", alarm_level_name((alarm_arm_level_t)ev->arg));
        pj_kbool(w, "no_entry_delay", (ev->arg2 & ALARM_ARM_NO_ENTRY_DELAY) != 0u);
        pj_kbool(w, "silent_exit", (ev->arg2 & ALARM_ARM_SILENT_EXIT) != 0u);
        break;
    case ALARM_EV_ARM_FAILED:
        pj_kstr(w, "result", alarm_result_name((alarm_result_t)ev->arg));
        break;
    case ALARM_EV_ALARM:
        pj_kstr(w, "cause", alarm_cause_name((alarm_cause_t)ev->arg));
        break;
    case ALARM_EV_EXIT_RESTART:
        pj_kbool(w, "automatic", ev->arg != 0u);
        break;
    case ALARM_EV_TROUBLE:
    case ALARM_EV_TROUBLE_RESTORE:
        trouble_fields(w, cfg, ev);
        break;
    case ALARM_EV_USER_CHANGED:
    case ALARM_EV_USER_DELETED:
    case ALARM_EV_USER_UPDATED:
        pj_knum(w, "target", ev->arg);
        pj_kstr(w, "target_name", user_name(cfg, ev->arg));
        break;
    case ALARM_EV_PROGRAM_ENTER:
        pj_kbool(w, "recovery", ev->arg == 1u);
        break;
    case ALARM_EV_PROGRAM_EXIT:
        pj_kbool(w, "timeout", ev->arg == 1u);
        break;
    case ALARM_EV_FACTORY_RESET:
        pj_kbool(w, "log_erased", (ev->arg & 1u) != 0u);
        break;
    case ALARM_EV_FIRMWARE_UPDATE:
        pj_kstr(w, "stage", alarm_fw_stage_name((alarm_fw_stage_t)ev->arg));
        break;
    case ALARM_EV_AUTOMATION_FAILED: /* docs/features/41 AUT-009; `automation` below */
        pj_kstr(w, "action", alarm_auto_action_name(ev->arg2));
        pj_kstr(w, "reason", alarm_auto_fail_name((alarm_auto_fail_t)ev->arg));
        break;
    case ALARM_EV_ARMING_PUT_OFF: { /* docs/features/41 AUT-013 */
        const int64_t until = alarm_put_off_until(ev);
        pj_kstr(w, "choice", alarm_put_off_name((alarm_put_off_t)ev->arg));
        if (until > 0) {
            pj_kint(w, "until", until);
        } else {
            pj_knull(w, "until");
        }
        break;
    }
    default:
        break;
    }
    if (ev->source.kind == (uint8_t)ALARM_SRC_AUTOMATION) {
        /* D30, AUT-009: which automation, by its trigger clause ("At 10:00 PM every day";
         * null when the panel can't say; the uid is source_id), and who saved it - the user
         * the engine acted as, or the hook's maker for a put-off, whose user is the person
         * who put it off. */
        notify_auto_info_t info;
        const notify_auto_found_t found = notify_auto_lookup(NULL, cfg, ev->source.id, &info);
        pj_kstr(w, "automation", auto_words(&info, found));
        const uint8_t maker = (found == NOTIFY_AUTO_FOUND && info.maker != 0u) ? info.maker
                              : (ev->type == ALARM_EV_ARMING_PUT_OFF)          ? 0u
                                                                               : ev->user;
        pj_kstr(w, "saved_by", user_name(cfg, maker));
    }
    pj_kbool(w, "silent", (ev->flags & ALARM_EVF_SILENT) != 0u);
    pj_kbool(w, "replayed", replayed);
}

void proto_event(pj_writer_t *w, const alarm_config_t *cfg, const alarm_event_t *ev,
                 bool replayed)
{
    pj_obj(w);
    proto_event_fields(w, cfg, ev, replayed);
    pj_obj_end(w);
}

/* ---- heartbeat --------------------------------------------------------------------------- */

void proto_heartbeat(pj_writer_t *w, const proto_heartbeat_t *hb)
{
    pj_obj(w);
    pj_kuint(w, "uptime_s", hb->uptime_s);
    pj_kstr(w, "fw", hb->fw);
    pj_kstr(w, "ip", (hb->ip != NULL && hb->ip[0] != '\0') ? hb->ip : NULL);
    if (hb->rssi != 0) {
        pj_kint(w, "rssi", hb->rssi);
    } else {
        pj_knull(w, "rssi");
    }
    pj_kuint(w, "free_heap_kb", hb->free_heap_kb);
    pj_kuint(w, "boot", hb->boot);
    pj_kuint(w, "unsent", hb->unsent);
    pj_obj_end(w);
}
