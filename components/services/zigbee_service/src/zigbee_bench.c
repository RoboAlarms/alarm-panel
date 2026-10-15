/*
 * zigbee_bench.c - the bench's tools on the service console (devices.md §12; BL-143 ZB-M6):
 * `zigbee device <id> signature` prints a device as a signature fixture row (testing.md §2) and
 * `zigbee trace on|off` prints every exchange with the radio for a replay (testing.md §7).
 *
 * The signature's descriptors are the raw ones the radio reported (every cluster id, vendor ones
 * too), captured as they pass to zbz_panel for the last ZBSVC_SIG_MAX devices set up since the
 * start; without them the panel's record gives what it kept (the clusters it knows). The trace
 * never prints a code: a keypad's Arm and Bypass carry one, and only their length is shown.
 * Both print real addresses: a row or a trace is scrubbed before it is committed (AGENTS.md
 * "Conventions"; testing.md §7).
 */
#include "zigbee_internal.h"

#if CONFIG_ALARM_ZSRC_ZIGBEE

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "radio_port.h"
#include "zigbee_zones/zbz_registry.h"
#include "zone_service.h"

#define ZBSVC_SIG_MAX 4u /* devices whose descriptors are kept (about 600 bytes each, PSRAM) */

typedef struct {
    bool used;
    uint32_t age; /* larger: newer; the oldest is replaced */
    uint64_t ieee;
    uint16_t nwk;
    bool node_known, power_known;
    zsrc_zb_node_desc_t node;
    zsrc_zb_power_desc_t power;
    uint8_t ep_count;
    zsrc_zb_simple_desc_t ep[ZSRC_ZB_EP_MAX];
} sig_t;

/* Written by the zigbee task under the zone lock, read by the console under it. */
static EXT_RAM_BSS_ATTR sig_t s_sig[ZBSVC_SIG_MAX];
static uint32_t s_sig_age;
/* The console's copies (its stack is small; one command at a time). */
static EXT_RAM_BSS_ATTR sig_t s_sig_copy;
static EXT_RAM_BSS_ATTR zbz_device_t s_dev_copy;

static atomic_bool s_trace;

/* ---- the descriptors, as they pass ------------------------------------------------------------ */

static sig_t *sig_slot(uint64_t ieee, bool make)
{
    sig_t *oldest = &s_sig[0];
    for (size_t i = 0; i < ZBSVC_SIG_MAX; i++) {
        if (s_sig[i].used && s_sig[i].ieee == ieee) {
            return &s_sig[i];
        }
        if (!s_sig[i].used || (oldest->used && s_sig[i].age < oldest->age)) {
            oldest = &s_sig[i];
        }
    }
    if (!make) {
        return NULL;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->used = true;
    oldest->ieee = ieee;
    return oldest;
}

void zbsvc_sig_capture(const zsrc_zb_report_t *r)
{
    if (r->ieee == 0u) {
        return;
    }
    const uint8_t kind = r->kind;
    if (kind != (uint8_t)ZSRC_ZB_REP_NODE_DESC && kind != (uint8_t)ZSRC_ZB_REP_POWER_DESC &&
        kind != (uint8_t)ZSRC_ZB_REP_SIMPLE_DESC) {
        return;
    }
    sig_t *s = sig_slot(r->ieee, true);
    s->age = ++s_sig_age;
    s->nwk = r->nwk;
    if (kind == (uint8_t)ZSRC_ZB_REP_NODE_DESC) {
        s->node = r->u.node;
        s->node_known = true;
        return;
    }
    if (kind == (uint8_t)ZSRC_ZB_REP_POWER_DESC) {
        s->power = r->u.power;
        s->power_known = true;
        return;
    }
    for (uint8_t i = 0u; i < s->ep_count; i++) {
        if (s->ep[i].endpoint == r->u.simple.endpoint) {
            s->ep[i] = r->u.simple; /* described again */
            return;
        }
    }
    if (s->ep_count < ZSRC_ZB_EP_MAX) {
        s->ep[s->ep_count++] = r->u.simple;
    }
}

/* ---- the signature row ------------------------------------------------------------------------ */

static void json_text(const char *s, size_t cap)
{
    putchar('"');
    for (size_t i = 0; i < cap && s[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == (unsigned char)'"' || c == (unsigned char)'\\') {
            printf("\\%c", (char)c);
        } else if (c < 0x20u || c == 0x7Fu) {
            printf("\\u%04x", (unsigned)c);
        } else {
            putchar((int)c);
        }
    }
    putchar('"');
}

static void json_ids(const uint16_t *ids, uint8_t count)
{
    putchar('[');
    for (uint8_t i = 0u; i < count; i++) {
        printf("%s\"0x%04X\"", i == 0u ? "" : ", ", (unsigned)ids[i]);
    }
    putchar(']');
}

/* ZSRC_ZB_CL_* bits back to cluster ids (zsrc_zigbee.h; the record keeps only these). */
static uint8_t bits_to_ids(uint16_t bits, uint16_t ids[ZSRC_ZB_EP_CLUSTERS_MAX])
{
    static const struct {
        uint16_t bit, id;
    } MAP[] = {{ZSRC_ZB_CL_BASIC, 0x0000u},     {ZSRC_ZB_CL_POWER_CFG, 0x0001u},
               {ZSRC_ZB_CL_IDENTIFY, 0x0003u},  {ZSRC_ZB_CL_ON_OFF, 0x0006u},
               {ZSRC_ZB_CL_OTA, 0x0019u},       {ZSRC_ZB_CL_POLL_CTRL, 0x0020u},
               {ZSRC_ZB_CL_TEMP, 0x0402u},      {ZSRC_ZB_CL_OCCUPANCY, 0x0406u},
               {ZSRC_ZB_CL_IAS_ZONE, 0x0500u},  {ZSRC_ZB_CL_IAS_ACE, 0x0501u},
               {ZSRC_ZB_CL_IAS_WD, 0x0502u},    {ZSRC_ZB_CL_DIAGNOSTICS, 0x0B05u},
               {ZSRC_ZB_CL_TUYA, 0xEF00u}};
    uint8_t n = 0u;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]) && n < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
        if ((bits & MAP[i].bit) != 0u) {
            ids[n++] = MAP[i].id;
        }
    }
    return n;
}

static const char *logical_word(uint8_t t)
{
    switch (t) {
    case ZSRC_ZB_LOGICAL_COORDINATOR: return "coordinator";
    case ZSRC_ZB_LOGICAL_ROUTER: return "router";
    case ZSRC_ZB_LOGICAL_END_DEVICE: return "end_device";
    default: return "unknown";
    }
}

static uint16_t record_zone_type(const zbz_device_t *d, uint8_t endpoint)
{
    for (uint8_t i = 0u; i < d->ep_count && i < ZBZ_EP_MAX; i++) {
        if (d->ep[i].endpoint == endpoint) {
            return d->ep[i].zone_type;
        }
    }
    return 0u;
}

static void print_endpoint_tail(uint16_t zone_type, bool has_ias)
{
    if (has_ias && zone_type != 0u) {
        printf(", \"zone_type\": \"0x%04X\"", (unsigned)zone_type);
    }
    printf("}");
}

int zbsvc_print_signature(uint16_t id)
{
    zbz_panel_t *panel = zbsvc_panel();
    if (panel == NULL) {
        printf("the sensor radio service is not running\n");
        return 1;
    }
    zone_service_lock();
    const zbz_device_t *d = zbz_reg_cget(&panel->reg, id);
    bool have = d != NULL;
    bool captured = false;
    if (have) {
        s_dev_copy = *d;
        const sig_t *s = sig_slot(d->ieee, false);
        captured = s != NULL && s->ep_count != 0u;
        if (s != NULL) {
            s_sig_copy = *s;
        } else {
            memset(&s_sig_copy, 0, sizeof(s_sig_copy));
        }
    }
    zone_service_unlock();
    if (!have) {
        printf("no device %u\n", (unsigned)id);
        return 1;
    }
    const zbz_device_t *r = &s_dev_copy;
    const sig_t *s = &s_sig_copy;
    printf("{\n  \"device\": ");
    json_text(r->model[0] != '\0' ? r->model : "?", sizeof(r->model));
    printf(",\n  \"tests\": \"?\",\n  \"provenance\": \"Captured with zigbee device %u signature"
           "%s. Replace the addresses with fictional ones before committing.\",\n",
           (unsigned)id,
           captured ? "" : " from the panel's record: only the clusters the panel knows (zigbee "
                           "setup reads the descriptors again for a full row)");
    printf("  \"ieee\": \"0x%016" PRIX64 "\",\n  \"nwk\": \"0x%04X\",\n", r->ieee,
           (unsigned)(s->nwk != 0u ? s->nwk : r->nwk));
    const uint8_t logical =
        s->node_known ? s->node.logical_type
        : r->node_type == (uint8_t)ZBZ_NODE_ROUTER     ? (uint8_t)ZSRC_ZB_LOGICAL_ROUTER
        : r->node_type == (uint8_t)ZBZ_NODE_END_DEVICE ? (uint8_t)ZSRC_ZB_LOGICAL_END_DEVICE
                                                       : 0xFFu;
    printf("  \"node\": {\"logical_type\": \"%s\", \"mac_caps\": \"0x%02X\", "
           "\"manufacturer_code\": %u, \"server_mask\": \"0x%04X\"},\n",
           logical_word(logical), (unsigned)(s->node_known ? s->node.mac_caps : r->mac_caps),
           (unsigned)(s->node_known ? s->node.manufacturer_code : r->manufacturer_code),
           (unsigned)(s->node_known ? s->node.server_mask : 0u));
    printf("  \"power_source\": %u,\n  \"basic\": {\"manufacturer\": ", (unsigned)r->power_source);
    json_text(r->manufacturer, sizeof(r->manufacturer));
    printf(", \"model\": ");
    json_text(r->model, sizeof(r->model));
    printf("},\n  \"endpoints\": [");
    uint16_t in[ZSRC_ZB_EP_CLUSTERS_MAX];
    uint16_t out[ZSRC_ZB_EP_CLUSTERS_MAX];
    const uint8_t count = captured ? s->ep_count : r->ep_count;
    for (uint8_t i = 0u; i < count && i < (captured ? ZSRC_ZB_EP_MAX : ZBZ_EP_MAX); i++) {
        printf("%s\n    {", i == 0u ? "" : ",");
        if (captured) {
            const zsrc_zb_simple_desc_t *e = &s->ep[i];
            printf("\"endpoint\": %u, \"profile\": \"0x%04X\", \"device_id\": \"0x%04X\", "
                   "\"in\": ",
                   (unsigned)e->endpoint, (unsigned)e->profile, (unsigned)e->device_id);
            const uint8_t ni = e->in_count > ZSRC_ZB_EP_CLUSTERS_MAX ? (uint8_t)ZSRC_ZB_EP_CLUSTERS_MAX
                                                                    : e->in_count;
            const uint8_t no = e->out_count > ZSRC_ZB_EP_CLUSTERS_MAX
                                   ? (uint8_t)ZSRC_ZB_EP_CLUSTERS_MAX
                                   : e->out_count;
            json_ids(e->in, ni);
            printf(", \"out\": ");
            json_ids(e->out, no);
            if (e->truncated) {
                printf(", \"truncated\": true");
            }
            bool ias = false;
            for (uint8_t k = 0u; k < ni; k++) {
                ias = ias || e->in[k] == 0x0500u;
            }
            print_endpoint_tail(record_zone_type(r, e->endpoint), ias);
        } else {
            const zbz_endpoint_t *e = &r->ep[i];
            printf("\"endpoint\": %u, \"profile\": \"0x%04X\", \"device_id\": \"0x%04X\", "
                   "\"in\": ",
                   (unsigned)e->endpoint, (unsigned)e->profile, (unsigned)e->device_id);
            json_ids(in, bits_to_ids(e->in_clusters, in));
            printf(", \"out\": ");
            json_ids(out, bits_to_ids(e->out_clusters, out));
            print_endpoint_tail(e->zone_type, (e->in_clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u);
        }
    }
    printf("\n  ]\n}\n");
    if (!captured) {
        printf("(no descriptors seen since the start: `zigbee setup %u` reads them again - wake a "
               "sleepy device - then ask again)\n",
               (unsigned)id);
    }
    return 0;
}

/* ---- the trace -------------------------------------------------------------------------------- */

void zbsvc_trace_set(bool on)
{
    atomic_store(&s_trace, on);
    radio_port_set_trace(on);
}

bool zbsvc_trace_on(void)
{
    return atomic_load(&s_trace);
}

void zbsvc_trace_out(const char *what, uint64_t ieee, uint8_t ep, uint32_t a, uint32_t tag)
{
    if (!atomic_load(&s_trace)) {
        return;
    }
    printf("zbtrace %" PRIu64 " > %016" PRIX64 " %s ep=%u a=0x%" PRIX32 " tag=%" PRIu32 "\n",
           zbsvc_now_ms(), ieee, what, (unsigned)ep, a, tag);
}

/* A keypad's Arm (0x00) and Bypass (0x01) carry its code: only the length is shown. */
static bool zcl_hides_code(const zsrc_zb_zcl_t *z)
{
    return z->cluster == ZSRC_ZB_ZCL_IAS_ACE && !z->server_to_client &&
           (z->command == 0x00u || z->command == 0x01u);
}

void zbsvc_trace_zcl(char dir, uint64_t ieee, uint8_t ep, const zsrc_zb_zcl_t *z, uint32_t tag)
{
    if (!atomic_load(&s_trace)) {
        return;
    }
    printf("zbtrace %" PRIu64 " %c %016" PRIX64 " zcl ep=%u cl=0x%04X cmd=0x%02X %s mfr=0x%04X "
           "tsn=%u tag=%" PRIu32 " len=%u",
           zbsvc_now_ms(), dir, ieee, (unsigned)ep, (unsigned)z->cluster, (unsigned)z->command,
           z->server_to_client ? "s2c" : "c2s", (unsigned)z->manufacturer, (unsigned)z->tsn, tag,
           (unsigned)z->len);
    if (zcl_hides_code(z)) {
        printf(" (a code: not shown)\n");
        return;
    }
    putchar(' ');
    for (uint8_t i = 0u; i < z->len && i < ZSRC_ZB_ZCL_PAYLOAD_MAX; i++) {
        printf("%02X", (unsigned)z->payload[i]);
    }
    putchar('\n');
}

void zbsvc_trace_report(uint64_t now, const zsrc_zb_report_t *r)
{
    if (!atomic_load(&s_trace)) {
        return;
    }
    if (r->kind == (uint8_t)ZSRC_ZB_REP_ZCL) {
        zbsvc_trace_zcl('<', r->ieee, r->endpoint, &r->u.zcl, r->tag);
        return;
    }
    printf("zbtrace %" PRIu64 " < %016" PRIX64 " rep=%u nwk=0x%04X ep=%u tag=%" PRIu32, now,
           r->ieee, (unsigned)r->kind, (unsigned)r->nwk, (unsigned)r->endpoint, r->tag);
    switch (r->kind) {
    case ZSRC_ZB_REP_JOINED:
        printf(" join=%u rejoin=%u parent=0x%04X key_pending=%u", (unsigned)r->u.joined.join,
               (unsigned)r->u.joined.rejoin, (unsigned)r->u.joined.parent,
               (unsigned)r->u.joined.key_pending);
        break;
    case ZSRC_ZB_REP_ANNOUNCE: printf(" mac=0x%02X", (unsigned)r->u.announce.mac_caps); break;
    case ZSRC_ZB_REP_LEFT:
        printf(" rejoin=%u asked=%u", (unsigned)r->u.left.rejoin, (unsigned)r->u.left.asked);
        break;
    case ZSRC_ZB_REP_JOIN_REFUSED: printf(" why=%u", (unsigned)r->u.refused.why); break;
    case ZSRC_ZB_REP_NODE_DESC:
        printf(" logical=%u mac=0x%02X mfr=%u mask=0x%04X", (unsigned)r->u.node.logical_type,
               (unsigned)r->u.node.mac_caps, (unsigned)r->u.node.manufacturer_code,
               (unsigned)r->u.node.server_mask);
        break;
    case ZSRC_ZB_REP_POWER_DESC:
        printf(" mode=%u sources=%u source=%u level=%u", (unsigned)r->u.power.current_mode,
               (unsigned)r->u.power.available_sources, (unsigned)r->u.power.current_source,
               (unsigned)r->u.power.level);
        break;
    case ZSRC_ZB_REP_ACTIVE_EP:
        printf(" eps=");
        for (uint8_t i = 0u; i < r->u.active.count && i < ZSRC_ZB_EP_MAX; i++) {
            printf("%s%u", i == 0u ? "" : ",", (unsigned)r->u.active.ep[i]);
        }
        break;
    case ZSRC_ZB_REP_SIMPLE_DESC: {
        const zsrc_zb_simple_desc_t *e = &r->u.simple;
        printf(" sep=%u profile=0x%04X dev=0x%04X in=", (unsigned)e->endpoint,
               (unsigned)e->profile, (unsigned)e->device_id);
        for (uint8_t i = 0u; i < e->in_count && i < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
            printf("%s%04X", i == 0u ? "" : ",", (unsigned)e->in[i]);
        }
        printf(" out=");
        for (uint8_t i = 0u; i < e->out_count && i < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
            printf("%s%04X", i == 0u ? "" : ",", (unsigned)e->out[i]);
        }
        break;
    }
    case ZSRC_ZB_REP_BASIC:
        printf(" have=0x%04X power=%u", (unsigned)r->u.basic.have,
               (unsigned)r->u.basic.power_source);
        break;
    case ZSRC_ZB_REP_LINK:
        printf(" lqi=%u rssi=%d", (unsigned)r->u.link.lqi, (int)r->u.link.rssi);
        break;
    case ZSRC_ZB_REP_DONE:
        printf(" step=%u result=%u status=0x%02X", (unsigned)r->u.done.step,
               (unsigned)r->u.done.result, (unsigned)r->u.done.zcl_status);
        break;
    case ZSRC_ZB_REP_NETWORK:
        printf(" channel=%u pan=0x%04X update=%u", (unsigned)r->u.network.channel,
               (unsigned)r->u.network.pan_id, (unsigned)r->u.network.update_id);
        break;
    default: break;
    }
    putchar('\n');
}

#endif /* CONFIG_ALARM_ZSRC_ZIGBEE */
