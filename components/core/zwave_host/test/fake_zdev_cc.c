/*
 * fake_zdev_cc.c - what a simulated device answers the panel's questions with (docs/features/36
 * command-classes.md §4; the interview, network.md §5-7), from its model (fake_model_t), built
 * with zwave_proto's codecs. An answer goes the way the question came: at its class, from its end
 * point; a supervised Set is answered with its Supervision Report. See fake_zdev.h.
 */
#include <string.h>

#include "fake_zdev.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_cc_fwu.h"
#include "zwave_proto/zw_cc_switch.h"

#define TAG_ANSWER 0x5A440010u

static uint8_t version_of(const fake_zdev_t *d, uint8_t cc)
{
    for (size_t i = 0; i < d->m.versions_n; i++) {
        if (d->m.versions[i].cc == cc) {
            return d->m.versions[i].version;
        }
    }
    return 0u;
}

static bool muted(fake_zdev_t *d, uint8_t cc, uint8_t cmd)
{
    fake_model_t *m = &d->m;
    if (m->mute_n == 0u || m->mute_cc != cc || (m->mute_cmd != 0u && m->mute_cmd != cmd)) {
        return false;
    }
    if (m->mute_n != FAKE_MUTE_ALWAYS) {
        m->mute_n--;
    }
    return true;
}

/* An answer, the way the question came. */
static void reply(fake_zdev_t *d, const zw_s2_rx_t *rx, const uint8_t *cmd, size_t n)
{
    uint8_t mc[64];
    if (n == 0u) {
        return;
    }
    if (rx->multi_channel) {
        uint8_t from = rx->destination_endpoint;
        if (d->m.wrong_ep_n > 0u && rx->command[0] == d->m.wrong_ep_cc &&
            rx->command[1] == d->m.wrong_ep_cmd) {
            d->m.wrong_ep_n--;
            from = from == 1u ? 2u : 1u; /* a fault: as if another end point answered */
        }
        n = zw_cc_wrap_multi_channel(mc, sizeof mc, from, rx->source_endpoint, false, cmd, n);
        cmd = mc;
    }
    d->answers++;
    (void)zw_s2_send(&d->s2, d->now, FAKE_ZDEV_PANEL, rx->security, cmd, n, TAG_ANSWER);
}

/* An answer the way the question came, sent at `at` (fake_zdev_poll), after those already
 * waiting. */
static void reply_later(fake_zdev_t *d, const zw_s2_rx_t *rx, const uint8_t *cmd, size_t n,
                        uint64_t at)
{
    uint8_t mc[64];
    if (n == 0u || d->later_n >= sizeof d->later / sizeof d->later[0]) {
        return;
    }
    if (rx->multi_channel) {
        n = zw_cc_wrap_multi_channel(mc, sizeof mc, rx->destination_endpoint, rx->source_endpoint,
                                     false, cmd, n);
        cmd = mc;
    }
    if (n == 0u || n > FAKE_ZDEV_CMD) {
        return;
    }
    d->later[d->later_n].at = at;
    d->later[d->later_n].cls = rx->security;
    d->later[d->later_n].len = n;
    memcpy(d->later[d->later_n].cmd, cmd, n);
    d->later_n++;
}

/* The lifeline group's Report in two parts (split_n): reports to follow 1 naming split_nodes,
 * then 0 with the group as it is - split_ms apart (the second lost split_lose_n times). */
static void split_report(fake_zdev_t *d, const zw_s2_rx_t *rx, bool mca, uint8_t group)
{
    fake_model_t *m = &d->m;
    uint8_t out[48];
    size_t n;
    if (mca) {
        const zw_cc_mc_association_report_t first = {group, 5u, 1u, m->split_nodes, m->split_n};
        n = zw_cc_mc_association_report_build(out, sizeof out, &first);
    } else {
        const zw_cc_association_report_t first = {group, 5u, 1u, m->split_nodes, m->split_n};
        n = zw_cc_association_report_build(out, sizeof out, &first);
    }
    reply_later(d, rx, out, n, d->now + m->split_ms);
    if (m->split_lose_n > 0u) {
        m->split_lose_n--;
        return;
    }
    if (mca) {
        const zw_cc_mc_association_report_t rest = {group, 5u, 0u, m->mca_dest,
                                                    m->read_back_empty ? 0u : m->mca_dest_n};
        n = zw_cc_mc_association_report_build(out, sizeof out, &rest);
    } else {
        const zw_cc_association_report_t rest = {group, 5u, 0u, m->assoc[0],
                                                 m->read_back_empty ? 0u : m->assoc_n[0]};
        n = zw_cc_association_report_build(out, sizeof out, &rest);
    }
    reply_later(d, rx, out, n, d->now + 2u * (uint64_t)m->split_ms);
}

/* The Sets the panel sends (a Set to answer with Application Status). */
static bool set_command(const uint8_t *c)
{
    return (c[0] == ZW_CC_CONFIGURATION && c[1] == ZW_CMD_CONFIGURATION_SET) ||
           (c[0] == ZW_CC_WAKE_UP && c[1] == ZW_CMD_WAKE_UP_INTERVAL_SET) ||
           ((c[0] == ZW_CC_ASSOCIATION || c[0] == ZW_CC_MULTI_CHANNEL_ASSOCIATION) &&
            c[1] == ZW_CMD_ASSOCIATION_SET) ||
           (c[0] == ZW_CC_NOTIFICATION && c[1] == ZW_CMD_NOTIFICATION_SET) ||
           (c[0] == ZW_CC_BASIC && c[1] == ZW_CMD_BASIC_SET);
}

/* A mask of the given types, bit t = type t (bit 0 unused): its length. */
static size_t type_mask(uint8_t *out, size_t cap, const uint8_t *types, size_t n, bool one_based)
{
    size_t len = 1u;
    memset(out, 0, cap);
    for (size_t i = 0; i < n; i++) {
        const unsigned bit = one_based ? types[i] - 1u : types[i];
        if (bit / 8u < cap) {
            out[bit / 8u] = (uint8_t)(out[bit / 8u] | 1u << (bit % 8u));
            len = bit / 8u + 1u > len ? bit / 8u + 1u : len;
        }
    }
    return len;
}

/* The end point a question is for (0: the device itself). */
static const fake_ep_t *endpoint(const fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    const uint8_t e = rx->multi_channel ? rx->destination_endpoint : 0u;
    return e >= 1u && e <= d->m.ep_n && e <= FAKE_ZDEV_EPS ? &d->m.eps[e - 1u] : NULL;
}

static size_t notification(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap,
                           bool *is_set)
{
    const fake_ep_t *ep = endpoint(d, rx);
    const uint8_t *types = ep != NULL ? ep->notif_types : d->m.notif_types;
    const size_t tn = ep != NULL ? ep->notif_n : d->m.notif_n;
    const uint8_t *c = rx->command;
    switch (c[1]) {
    case ZW_CMD_NOTIFICATION_SUPPORTED_GET: {
        uint8_t mask[4];
        const size_t ml = type_mask(mask, sizeof mask, types, tn, false);
        const zw_cc_notification_supported_report_t r = {false, mask, ml};
        return zw_cc_notification_supported_report_build(out, cap, &r);
    }
    case ZW_CMD_NOTIFICATION_EVENT_SUPPORTED_GET: {
        zw_cc_notification_event_supported_get_t g;
        static const uint8_t DEFAULT_EVENTS[3] = {0xFEu, 0xFFu, 0xFFu}; /* events 1..23 */
        if (!zw_cc_notification_event_supported_get_parse(c, rx->len, &g)) {
            return 0u;
        }
        const uint8_t *events = DEFAULT_EVENTS;
        size_t events_len = sizeof DEFAULT_EVENTS;
        for (size_t i = 0; i < d->m.notif_events_n; i++) {
            if (d->m.notif_events[i].type == g.notification_type) {
                events = d->m.notif_events[i].mask;
                events_len = d->m.notif_events[i].len;
                break;
            }
        }
        const zw_cc_notification_event_supported_report_t r = {g.notification_type, events,
                                                               events_len};
        return zw_cc_notification_event_supported_report_build(out, cap, &r);
    }
    case ZW_CMD_NOTIFICATION_GET: {
        zw_cc_notification_get_t g;
        if (!zw_cc_notification_get_parse(c, rx->len, &g)) {
            return 0u;
        }
        uint8_t status = ZW_NOTIFICATION_STATUS_OFF;
        for (size_t i = 0; i < tn && ep == NULL; i++) {
            if (types[i] == g.notification_type && !d->m.pull) {
                status = d->m.notif_status[i];
            }
        }
        if (ep != NULL && !d->m.pull) {
            status = ZW_NOTIFICATION_STATUS_ON;
        }
        const zw_cc_notification_report_t r = {false, 0u, 0u, status, g.notification_type, 0u,
                                               NULL, 0u, false, 0u};
        return zw_cc_notification_report_build(out, cap, &r);
    }
    case ZW_CMD_NOTIFICATION_SET: {
        zw_cc_notification_set_t s;
        *is_set = true;
        if (zw_cc_notification_set_parse(c, rx->len, &s) && !d->m.pull) {
            for (size_t i = 0; i < tn && ep == NULL; i++) {
                if (types[i] == s.notification_type) {
                    d->m.notif_status[i] = s.status;
                }
            }
        }
        return 0u;
    }
    default:
        return 0u;
    }
}

static size_t association(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap,
                          bool *is_set)
{
    fake_model_t *m = &d->m;
    const uint8_t *c = rx->command;
    const bool mca = c[0] == ZW_CC_MULTI_CHANNEL_ASSOCIATION;
    switch (c[1]) {
    case ZW_CMD_ASSOCIATION_SET:
        *is_set = true;
        if (mca) {
            zw_cc_mc_association_set_t s;
            if (zw_cc_mc_association_set_parse(c, rx->len, &s) && s.group == 1u &&
                s.destinations_len <= sizeof m->mca_dest) {
                memcpy(m->mca_dest, s.destinations, s.destinations_len);
                m->mca_dest_n = s.destinations_len;
            }
        } else {
            zw_cc_association_set_t s;
            if (zw_cc_association_set_parse(c, rx->len, &s) && s.group >= 1u && s.group <= 4u &&
                s.group <= m->groups) {
                uint8_t *g = m->assoc[s.group - 1u];
                size_t *gn = &m->assoc_n[s.group - 1u];
                for (size_t i = 0; i < s.node_id_count && *gn < sizeof m->assoc[0]; i++) {
                    if (memchr(g, s.node_ids[i], *gn) == NULL) {
                        g[(*gn)++] = s.node_ids[i];
                    }
                }
            }
        }
        return 0u;
    case ZW_CMD_ASSOCIATION_GET: {
        const uint8_t group = rx->len > 2u ? c[2] : 0u;
        if (group == 1u && m->split_n > 0u) {
            split_report(d, rx, mca, group);
            return 0u;
        }
        if (mca) {
            const zw_cc_mc_association_report_t r = {group, 5u, 0u, m->mca_dest,
                                                     m->read_back_empty ? 0u : m->mca_dest_n};
            return zw_cc_mc_association_report_build(out, cap, &r);
        }
        const bool known = group >= 1u && group <= 4u;
        const zw_cc_association_report_t r = {
            group, 5u, 0u, known ? m->assoc[group - 1u] : NULL,
            known && !m->read_back_empty ? m->assoc_n[group - 1u] : 0u};
        return zw_cc_association_report_build(out, cap, &r);
    }
    case ZW_CMD_ASSOCIATION_SUPPORTED_GROUPINGS_GET:
        if (mca) {
            const zw_cc_mc_association_supported_groupings_report_t r = {m->mca_groups};
            return zw_cc_mc_association_supported_groupings_report_build(out, cap, &r);
        } else {
            const zw_cc_association_supported_groupings_report_t r = {m->groups};
            return zw_cc_association_supported_groupings_report_build(out, cap, &r);
        }
    default:
        return 0u;
    }
}

static size_t agi(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap)
{
    const uint8_t *c = rx->command;
    if (c[1] == ZW_CMD_AGI_INFO_GET) {
        const uint8_t g[7] = {1u, 0u, (uint8_t)(d->m.agi_profile >> 8), (uint8_t)d->m.agi_profile,
                              0u, 0u, 0u};
        const zw_cc_agi_info_report_t r = {false, false, 1u, g, sizeof g};
        return zw_cc_agi_info_report_build(out, cap, &r);
    }
    if (c[1] == ZW_CMD_AGI_COMMAND_LIST_GET) {
        const zw_cc_agi_command_list_report_t r = {rx->len > 3u ? c[3] : 1u, d->m.agi_cmds,
                                                   d->m.agi_cmds_n};
        return zw_cc_agi_command_list_report_build(out, cap, &r);
    }
    return 0u;
}

static size_t wake_up(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap, bool *is_set)
{
    fake_model_t *m = &d->m;
    const uint8_t *c = rx->command;
    switch (c[1]) {
    case ZW_CMD_WAKE_UP_INTERVAL_CAPABILITIES_GET: {
        const zw_cc_wake_up_interval_capabilities_report_t r = {m->wake_min, m->wake_max,
                                                                m->wake_default, m->wake_step,
                                                                false, false};
        return zw_cc_wake_up_interval_capabilities_report_build(out, cap, &r);
    }
    case ZW_CMD_WAKE_UP_INTERVAL_GET: {
        const zw_cc_wake_up_interval_report_t r = {m->wake_interval, m->wake_node};
        return zw_cc_wake_up_interval_report_build(out, cap, &r);
    }
    case ZW_CMD_WAKE_UP_INTERVAL_SET: {
        zw_cc_wake_up_interval_set_t s;
        *is_set = true;
        if (zw_cc_wake_up_interval_set_parse(c, rx->len, &s)) {
            m->wake_interval = s.interval_seconds;
            m->wake_node = s.node_id_to_notify;
            d->next_wake = s.interval_seconds != 0u
                               ? d->now + (uint64_t)s.interval_seconds * 1000u
                               : 0u;
        }
        return 0u;
    }
    case ZW_CMD_WAKE_UP_NO_MORE_INFORMATION:
        d->nmi_heard++;
        d->awake_until = d->now; /* back to sleep */
        return 0u;
    default:
        return 0u;
    }
}

static size_t sensors(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap)
{
    const fake_ep_t *ep = endpoint(d, rx);
    const uint8_t *c = rx->command;
    uint8_t mask[4];
    if (c[0] == ZW_CC_BINARY_SENSOR) {
        const uint8_t *types = ep != NULL ? ep->binary_types : d->m.binary_types;
        const size_t tn = ep != NULL ? ep->binary_n : d->m.binary_n;
        if (c[1] == ZW_CMD_BINARY_SENSOR_SUPPORTED_GET) {
            const zw_cc_binary_sensor_supported_report_t r = {
                mask, type_mask(mask, sizeof mask, types, tn, false)};
            return zw_cc_binary_sensor_supported_report_build(out, cap, &r);
        }
        if (c[1] == ZW_CMD_BINARY_SENSOR_GET) {
            const bool typed = rx->len > 2u;
            const zw_cc_binary_sensor_report_t r = {false, typed, typed ? c[2] : 0u};
            return zw_cc_binary_sensor_report_build(out, cap, &r);
        }
        return 0u;
    }
    const bool temperature = ep != NULL ? ep->temperature : d->m.temperature;
    if (c[1] == ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_GET) {
        const uint8_t t = ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE;
        const zw_cc_multilevel_sensor_supported_sensor_report_t r = {
            mask, type_mask(mask, sizeof mask, &t, temperature ? 1u : 0u, true)};
        return zw_cc_multilevel_sensor_supported_sensor_report_build(out, cap, &r);
    }
    if (c[1] == ZW_CMD_MULTILEVEL_SENSOR_GET) {
        const zw_cc_multilevel_sensor_report_t r = {ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE, 1u,
                                                    0u, 2u, d->m.temp_c10};
        return zw_cc_multilevel_sensor_report_build(out, cap, &r);
    }
    return 0u;
}

static size_t device_info(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap)
{
    const fake_model_t *m = &d->m;
    const uint8_t *c = rx->command;
    switch (c[0]) {
    case ZW_CC_MANUFACTURER_SPECIFIC: {
        const zw_cc_manufacturer_specific_report_t r = {m->manufacturer, m->product_type,
                                                        m->product_id};
        return c[1] == ZW_CMD_MANUFACTURER_SPECIFIC_GET
                   ? zw_cc_manufacturer_specific_report_build(out, cap, &r)
                   : 0u;
    }
    case ZW_CC_VERSION:
        if (c[1] == ZW_CMD_VERSION_GET) {
            const zw_cc_version_report_t r = {0x03u, 7u, 18u, m->fw_major, m->fw_minor,
                                              false, 0u, NULL, 0u};
            return zw_cc_version_report_build(out, cap, &r);
        }
        if (c[1] == ZW_CMD_VERSION_COMMAND_CLASS_GET && rx->len > 2u) {
            const zw_cc_version_command_class_report_t r = {c[2], version_of(d, c[2])};
            return zw_cc_version_command_class_report_build(out, cap, &r);
        }
        return 0u;
    case ZW_CC_ZWAVE_PLUS_INFO: {
        const zw_cc_zwave_plus_info_report_t r = {2u, m->zwp_role, m->zwp_node_type, m->icon,
                                                  m->icon};
        return c[1] == ZW_CMD_ZWAVE_PLUS_INFO_GET ? zw_cc_zwave_plus_info_report_build(out, cap, &r)
                                                  : 0u;
    }
    case ZW_CC_BATTERY: {
        zw_cc_battery_report_t r;
        memset(&r, 0, sizeof r);
        r.level = m->battery;
        return c[1] == ZW_CMD_BATTERY_GET ? zw_cc_battery_report_build(out, cap, &r) : 0u;
    }
    case ZW_CC_MULTI_CHANNEL:
        if (c[1] == ZW_CMD_MULTI_CHANNEL_END_POINT_GET) {
            const zw_cc_multi_channel_end_point_report_t r = {false, false, m->ep_n, false, 0u};
            return zw_cc_multi_channel_end_point_report_build(out, cap, &r);
        }
        if (c[1] == ZW_CMD_MULTI_CHANNEL_CAPABILITY_GET && rx->len > 2u && c[2] >= 1u &&
            c[2] <= m->ep_n && c[2] <= FAKE_ZDEV_EPS) {
            const fake_ep_t *e = &m->eps[c[2] - 1u];
            const zw_cc_multi_channel_capability_report_t r = {false, c[2], e->generic,
                                                               e->specific, e->ccs, e->cc_n};
            return zw_cc_multi_channel_capability_report_build(out, cap, &r);
        }
        return 0u;
    case ZW_S2_CC: {
        const fake_ep_t *e = endpoint(d, rx);
        return c[1] == ZW_S2_COMMANDS_SUPPORTED_GET
                   ? zw_s2_commands_supported_report_build(
                         out, cap, e != NULL ? e->secure_ccs : m->secure_ccs,
                         e != NULL ? e->secure_n : m->secure_n)
                   : 0u;
    }
    default:
        return 0u;
    }
}

/* ZW-034: a switch, a dimmer, a relay: Set and Get at the device or an end point. */
static size_t switches(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap,
                       bool *is_set)
{
    const uint8_t *c = rx->command;
    const uint8_t ep = rx->multi_channel ? rx->destination_endpoint : 0u;
    if (ep > FAKE_ZDEV_EPS) {
        return 0u;
    }
    zw_cc_switch_set_t s;
    if (zw_cc_switch_set_parse(c, rx->len, c[0], &s)) {
        *is_set = true;
        if (d->m.sw_ignore) {
            return 0u;
        }
        d->m.sw[ep] = s.value == ZW_SWITCH_ON && c[0] == (uint8_t)ZW_CC_MULTILEVEL_SWITCH
                          ? ZW_SWITCH_LEVEL_MAX
                          : s.value;
        d->m.sw_sets++;
        if (d->m.sw_report) {
            const zw_cc_switch_report_t r = {d->m.sw[ep], false, 0u, 0u};
            uint8_t rep[6];
            reply_later(d, rx, rep, zw_cc_switch_report_build(rep, sizeof rep, c[0], &r),
                        d->now + 30u);
        }
        return 0u;
    }
    if (zw_cc_switch_get_parse(c, rx->len, c[0])) {
        const zw_cc_switch_report_t r = {d->m.sw[ep], false, 0u, 0u};
        return zw_cc_switch_report_build(out, cap, c[0], &r);
    }
    return 0u;
}

/* ZW-033: a device taking its new software - its meta data, the Request's answer, then its own
 * Gets for fragments (two at a time) until the last, its image's checksum checked, its verdict. */
static void fw_ask(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    const zw_fwu_get_t g = {2u, d->fw.next};
    uint8_t out[8];
    reply_later(d, rx, out, zw_fwu_get_build(out, sizeof out, &g), d->now + 5u);
}

static size_t firmware(fake_zdev_t *d, const zw_s2_rx_t *rx, uint8_t *out, size_t cap)
{
    const uint8_t *c = rx->command;
    if (!d->fw.on) {
        return 0u;
    }
    switch (c[1]) {
    case ZW_FWU_MD_GET: {
        zw_fwu_md_t md;
        memset(&md, 0, sizeof md);
        md.manufacturer_id = d->m.manufacturer;
        md.firmware_id = d->fw.firmware_id;
        md.checksum = 0x1234u;
        md.upgradable = !d->fw.not_upgradable;
        md.max_fragment = d->fw.max_fragment;
        md.hardware = 1u;
        md.supports_activation = d->fw.activation;
        return zw_fwu_md_report_build(out, cap, &md);
    }
    case ZW_FWU_REQUEST_GET: {
        zw_fwu_request_t q;
        uint8_t st = ZW_FWU_REQ_OK;
        if (!zw_fwu_request_get_parse(c, rx->len, &q)) {
            return 0u;
        }
        if (d->fw.refuse_set) {
            st = d->fw.refuse;
        } else if (q.manufacturer_id != d->m.manufacturer || q.firmware_id != d->fw.firmware_id) {
            st = ZW_FWU_REQ_WRONG_ID;
        } else if (d->fw.max_fragment != 0u && q.fragment_size > d->fw.max_fragment) {
            st = ZW_FWU_REQ_FRAGMENT_TOO_BIG;
        }
        if (st == ZW_FWU_REQ_OK) {
            d->fw.active = true;
            d->fw.fragment = q.fragment_size;
            d->fw.checksum = q.checksum;
            d->fw.next = 1u;
            d->fw.len = 0u;
            d->fw.status_sent = false;
            fw_ask(d, rx);
        }
        const uint8_t rep[3] = {ZW_CC_FIRMWARE_UPDATE, ZW_FWU_REQUEST_REPORT, st};
        memcpy(out, rep, sizeof rep);
        return sizeof rep;
    }
    case ZW_FWU_REPORT: {
        uint16_t number = 0u;
        bool last = false;
        const uint8_t *data = NULL;
        size_t n = 0u;
        if (!d->fw.active) {
            return 0u;
        }
        if (!zw_fwu_report_parse(c, rx->len, true, &number, &last, &data, &n)) {
            d->fw.bad++;
            return 0u;
        }
        if (d->fw.heard_n < sizeof d->fw.heard / sizeof d->fw.heard[0]) {
            d->fw.heard[d->fw.heard_n++] = number;
        }
        if (number == d->fw.drop_at) {
            d->fw.drop_at = 0u; /* lost on the way: asked for again below */
            fw_ask(d, rx);
            return 0u;
        }
        if (number != d->fw.next) {
            d->fw.unexpected++;
            return 0u;
        }
        const size_t at = (size_t)(number - 1u) * d->fw.fragment;
        if (at + n > sizeof d->fw.image) {
            return 0u;
        }
        memcpy(&d->fw.image[at], data, n);
        d->fw.len = at + n;
        d->fw.fragments++;
        d->fw.next++;
        if (last) {
            d->fw.active = false;
            const bool ok = zw_fwu_image_checksum(d->fw.image, d->fw.len) == d->fw.checksum;
            const uint8_t st = !ok                 ? ZW_FWU_STATUS_CHECKSUM
                               : d->fw.activation  ? ZW_FWU_STATUS_WAIT_ACTIVATE
                               : d->fw.verdict != 0u ? d->fw.verdict
                                                     : ZW_FWU_STATUS_OK_RESTART;
            if (ok && !d->fw.activation &&
                (st == ZW_FWU_STATUS_OK_RESTART || st == ZW_FWU_STATUS_OK_NO_RESTART)) {
                d->fw.updates++;
                if (d->fw.after_major != 0u || d->fw.after_minor != 0u) {
                    d->m.fw_major = d->fw.after_major;
                    d->m.fw_minor = d->fw.after_minor;
                }
            }
            d->fw.status = st;
            d->fw.status_sent = true;
            return zw_fwu_status_report_build(out, cap, st, d->fw.wait_s);
        }
        if (d->fw.quiet_after != 0u && number >= d->fw.quiet_after) {
            return 0u; /* it stops asking */
        }
        if ((number & 1u) == 0u) {
            fw_ask(d, rx); /* two taken: the next two */
        }
        return 0u;
    }
    case ZW_FWU_ACTIVATION_SET: {
        zw_fwu_activation_t a;
        memset(&a, 0, sizeof a);
        a.manufacturer_id = d->m.manufacturer;
        a.firmware_id = d->fw.firmware_id;
        a.checksum = d->fw.checksum;
        a.hardware = 0xFFu; /* the Set's layout without it: the Report's status next */
        a.status = 0xFFu;
        d->fw.updates++;
        uint8_t rep[16];
        const size_t k = zw_fwu_activation_set_build(rep, sizeof rep, &a);
        if (k < 2u || k > cap) {
            return 0u;
        }
        memcpy(out, rep, k);
        out[1] = ZW_FWU_ACTIVATION_REPORT; /* the same layout, its status after */
        out[k] = a.status;
        return k + 1u;
    }
    default:
        return 0u;
    }
}

static void count(fake_zdev_t *d, uint8_t cc, uint8_t cmd)
{
    if (cc == d->watch_cc && cmd == d->watch_cmd && d->watch_n < 32u) {
        d->watch_at[d->watch_n++] = d->now;
    }
    for (size_t i = 0; i < d->asked_k; i++) {
        if (d->asked[i].cc == cc && d->asked[i].cmd == cmd) {
            d->asked[i].n++;
            return;
        }
    }
    if (d->asked_k < sizeof d->asked / sizeof d->asked[0]) {
        d->asked[d->asked_k].cc = cc;
        d->asked[d->asked_k].cmd = cmd;
        d->asked[d->asked_k].n = 1u;
        d->asked_k++;
    }
}

void fake_zdev_notification_events(fake_zdev_t *d, uint8_t type, const uint8_t *events, size_t n)
{
    fake_model_t *m = &d->m;
    for (size_t i = 0; i < m->notif_events_n; i++) {
        if (m->notif_events[i].type == type) {
            m->notif_events[i].len = type_mask(m->notif_events[i].mask,
                                               sizeof m->notif_events[i].mask, events, n, false);
            return;
        }
    }
    if (m->notif_events_n < sizeof m->notif_events / sizeof m->notif_events[0]) {
        m->notif_events[m->notif_events_n].type = type;
        m->notif_events[m->notif_events_n].len =
            type_mask(m->notif_events[m->notif_events_n].mask,
                     sizeof m->notif_events[m->notif_events_n].mask, events, n, false);
        m->notif_events_n++;
    }
}

unsigned fake_zdev_asked(const fake_zdev_t *d, uint8_t cc, uint8_t cmd)
{
    for (size_t i = 0; i < d->asked_k; i++) {
        if (d->asked[i].cc == cc && d->asked[i].cmd == cmd) {
            return d->asked[i].n;
        }
    }
    return 0u;
}

bool fake_zdev_answer(fake_zdev_t *d, const zw_s2_rx_t *rx)
{
    const uint8_t *c = rx->command;
    count(d, c[0], rx->len > 1u ? c[1] : 0u);
    if (!d->m.answers) {
        return false;
    }
    if (rx->len < 2u) {
        return rx->len == 1u && c[0] == ZW_CC_NO_OPERATION; /* a ping: its ACK is the answer */
    }
    const fake_ep_t *ep = endpoint(d, rx);
    const uint8_t *secure = ep != NULL ? ep->secure_ccs : d->m.secure_ccs;
    const size_t secure_n = ep != NULL ? ep->secure_n : d->m.secure_n;
    if (d->have != 0u && rx->security == ZW_SEC_NONE && memchr(secure, c[0], secure_n) != NULL) {
        return true; /* a secure device ignores its (or the end point's) secure CCs in the clear
                      * (security.md §8) */
    }
    if (muted(d, c[0], c[1])) {
        return true;
    }
    if (d->m.app_status_n > 0u && c[0] == d->m.app_status_cc && set_command(c)) {
        /* command-classes.md §4.25: busy, or refused - the Set is not carried out, and a
         * supervised one gets no Supervision Report */
        uint8_t st[4];
        size_t k;
        d->m.app_status_n--;
        if (d->m.app_status_cmd == ZW_CMD_APPLICATION_BUSY) {
            const zw_cc_application_busy_t b = {d->m.app_busy_status, d->m.app_busy_wait_s};
            k = zw_cc_application_busy_build(st, sizeof st, &b);
        } else {
            const zw_cc_application_rejected_request_t r = {0u};
            k = zw_cc_application_rejected_request_build(st, sizeof st, &r);
        }
        if (rx->supervised) {
            d->spv_last = rx->supervision_session;
        }
        reply(d, rx, st, k);
        return true;
    }
    uint8_t out[64];
    size_t n = 0u;
    bool is_set = false;
    switch (c[0]) {
    case ZW_CC_NOTIFICATION:
        n = notification(d, rx, out, sizeof out, &is_set);
        break;
    case ZW_CC_ASSOCIATION:
    case ZW_CC_MULTI_CHANNEL_ASSOCIATION:
        n = association(d, rx, out, sizeof out, &is_set);
        break;
    case ZW_CC_ASSOCIATION_GROUP_INFORMATION:
        n = agi(d, rx, out, sizeof out);
        break;
    case ZW_CC_WAKE_UP:
        n = wake_up(d, rx, out, sizeof out, &is_set);
        break;
    case ZW_CC_BINARY_SENSOR:
    case ZW_CC_MULTILEVEL_SENSOR:
        n = sensors(d, rx, out, sizeof out);
        break;
    case ZW_CC_ALARM_SENSOR:
        if (c[1] == ZW_CMD_ALARM_SENSOR_SUPPORTED_GET && d->m.alarm_bad_report) {
            static const uint8_t BAD[] = {ZW_CC_ALARM_SENSOR, ZW_CMD_ALARM_SENSOR_SUPPORTED_REPORT,
                                          5u, 0x16u}; /* 5 bytes claimed, 1 sent */
            memcpy(out, BAD, sizeof BAD);
            n = sizeof BAD;
        } else if (c[1] == ZW_CMD_ALARM_SENSOR_SUPPORTED_GET) {
            uint8_t mask[4];
            const zw_cc_alarm_sensor_supported_report_t r = {
                mask, type_mask(mask, sizeof mask, d->m.alarm_types, d->m.alarm_n, false)};
            n = zw_cc_alarm_sensor_supported_report_build(out, sizeof out, &r);
        } else if (c[1] == ZW_CMD_ALARM_SENSOR_GET && rx->len > 2u) {
            const bool on = c[2] < 32u && (d->m.alarm_active & (1u << c[2])) != 0u;
            const zw_cc_alarm_sensor_report_t r = {
                0u, c[2], (uint8_t)(on ? ZW_ALARM_SENSOR_STATE_ALARM : ZW_ALARM_SENSOR_STATE_IDLE),
                0u};
            n = zw_cc_alarm_sensor_report_build(out, sizeof out, &r);
        }
        break;
    case ZW_CC_BINARY_SWITCH:
    case ZW_CC_MULTILEVEL_SWITCH:
        n = switches(d, rx, out, sizeof out, &is_set);
        break;
    case ZW_CC_FIRMWARE_UPDATE:
        n = firmware(d, rx, out, sizeof out);
        is_set = d->fw.on && c[1] == ZW_FWU_REPORT; /* a fragment: nothing to answer */
        break;
    case ZW_CC_CONFIGURATION: {
        zw_cc_configuration_set_t s;
        is_set = c[1] == ZW_CMD_CONFIGURATION_SET;
        if (is_set && zw_cc_configuration_set_parse(c, rx->len, &s) && d->m.config_n < 4u) {
            d->m.config[d->m.config_n].parameter = s.parameter;
            d->m.config[d->m.config_n].value = s.value;
            d->m.config_n++;
        }
        break;
    }
    default:
        n = device_info(d, rx, out, sizeof out);
        break;
    }
    if (rx->supervised) {
        d->supervised++;
        /* command-classes.md §4.14: the Report with the Get's session, at the Get's class - or,
         * as the model says, a stale session (the Get before's) or another one */
        uint8_t session = rx->supervision_session;
        if (is_set && d->m.spv_wrong_n > 0u && c[0] == d->m.spv_wrong_cc) {
            d->m.spv_wrong_n--;
            session = d->m.spv_stale ? d->spv_last : (uint8_t)((unsigned)session % 63u + 1u);
        }
        d->spv_last = rx->supervision_session;
        const zw_cc_supervision_report_t r = {
            false, false, session,
            !is_set ? ZW_SUPERVISION_STATUS_NO_SUPPORT
            : d->m.supervision_fail ? ZW_SUPERVISION_STATUS_FAIL
                                    : ZW_SUPERVISION_STATUS_SUCCESS,
            0u};
        uint8_t sr[8];
        const size_t k = zw_cc_supervision_report_build(sr, sizeof sr, &r);
        d->answers++;
        (void)zw_s2_send(&d->s2, d->now, FAKE_ZDEV_PANEL, rx->security, sr, k, TAG_ANSWER);
        return true;
    }
    reply(d, rx, out, n);
    return n > 0u || is_set;
}
