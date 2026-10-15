/*
 * zw_keyctl.c - which Z-Wave keypads may arm and disarm the panel. See zw_keyctl.h.
 */
#include "zwave_zones/zw_keyctl.h"

#include <string.h>

#include "zwave_proto/zw_crc16.h"

_Static_assert(ALARM_MAX_DEVICES >= ZWKC_SLOTS, "a device number for every keypad slot");

static bool secure_entry_control(const zw_node_record_t *n, bool *listed)
{
    *listed = false;
    for (size_t i = 0; i < n->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (n->ccs[i].cc == (uint8_t)ZW_CC_ENTRY_CONTROL) {
            *listed = true;
            if (n->ccs[i].secure) {
                return true;
            }
        }
    }
    return false;
}

static uint8_t highest(const zw_node_record_t *n)
{
    return (uint8_t)((n->security_classes & ZW_S2_KEY_ACCESS) != 0u ? ZW_SEC_S2_ACCESS
                                                                     : ZW_SEC_S2_AUTH);
}

void zwkc_init(zwkc_t *k, const zwk_port_t *port)
{
    memset(k, 0, sizeof(*k));
    if (port != NULL) {
        k->port = *port;
    }
}

zwkc_fit_t zwkc_fit(const zw_net_t *net, uint16_t node)
{
    const zw_node_record_t *n = zw_node_find_const(&net->nodes, node);
    if (n == NULL) {
        return ZWKC_FIT_NONE;
    }
    bool listed = false;
    const bool secure = secure_entry_control(n, &listed);
    if (n->interview_step != ZW_IV_STEP_DONE || n->security_classes == ZW_NODE_SECURITY_UNKNOWN) {
        return listed ? ZWKC_FIT_NOT_READY : ZWKC_FIT_NONE;
    }
    if (!listed) {
        return ZWKC_FIT_NONE;
    }
    if (!secure || (n->security_classes & (ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)) == 0u) {
        return ZWKC_FIT_NOT_SECURE;
    }
    return ZWKC_FIT_OK;
}

size_t zwkc_slot_of(const zwkc_t *k, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZWKC_SLOTS; i++) {
        if (k->slot[i].binding.partition != 0u && k->slot[i].binding.node == node) {
            return i;
        }
    }
    return ZWKC_SLOTS;
}

size_t zwkc_slot_for(const zwkc_t *k, uint16_t node)
{
    const size_t own = zwkc_slot_of(k, node);
    if (own < ZWKC_SLOTS) {
        return own;
    }
    for (size_t i = 0; i < ZWKC_SLOTS; i++) {
        if (k->slot[i].binding.partition == 0u) {
            return i;
        }
    }
    return ZWKC_SLOTS;
}

/* The slot's adapter from its binding: false when the binding can't be one. */
static bool arm_adapter(zwkc_t *k, size_t slot)
{
    const zwkc_binding_t *b = &k->slot[slot].binding;
    const zwk_binding_t zb = {.home_id = b->home,
                              .generation = b->generation,
                              .node = b->node,
                              .device = ZWKC_DEVICE(slot),
                              .endpoint = b->endpoint,
                              .partition = b->partition,
                              .panic_mask = b->panic_mask,
                              .security = (zw_sec_class_t)b->security};
    return zwk_init(&k->slot[slot].adapter, &zb, &k->port);
}

bool zwkc_assign(zwkc_t *k, size_t slot, const zw_net_t *net, uint16_t node, uint8_t partition,
                 uint8_t panic_mask)
{
    if (slot >= ZWKC_SLOTS || partition > ALARM_MAX_PARTITIONS) {
        return false;
    }
    if (partition == 0u) {
        memset(&k->slot[slot], 0, sizeof(k->slot[slot])); /* its recent results go too */
        return true;
    }
    const zw_node_record_t *n = zw_node_find_const(&net->nodes, node);
    if (n == NULL || node > 232u || zwkc_fit(net, node) != ZWKC_FIT_OK || net->keys_home_id == 0u ||
        !net->checked || net->radio != ZW_NET_RADIO_OURS) {
        return false;
    }
    const uint8_t allowed = (uint8_t)((1u << ALARM_PANIC_FIRE) | (1u << ALARM_PANIC_POLICE) |
                                      (1u << ALARM_PANIC_MEDICAL));
    if ((panic_mask & (uint8_t)~allowed) != 0u) {
        return false;
    }
    for (size_t i = 0; i < ZWKC_SLOTS; i++) {
        if (i != slot && k->slot[i].binding.partition != 0u && k->slot[i].binding.node == node) {
            return false; /* one keypad, one slot */
        }
    }
    const zwkc_slot_t before = k->slot[slot];
    memset(&k->slot[slot], 0, sizeof(k->slot[slot]));
    k->slot[slot].binding = (zwkc_binding_t){net->keys_home_id, node, n->generation, 0u,
                                             partition, highest(n), panic_mask};
    if (!arm_adapter(k, slot)) {
        k->slot[slot] = before;
        return false;
    }
    return true;
}

bool zwkc_live(const zwkc_t *k, size_t slot, const zw_net_t *net)
{
    if (slot >= ZWKC_SLOTS || k->slot[slot].binding.partition == 0u ||
        !k->slot[slot].adapter.enabled || !net->checked || net->radio != ZW_NET_RADIO_OURS) {
        return false;
    }
    const zwkc_binding_t *b = &k->slot[slot].binding;
    const zw_node_record_t *n = zw_node_find_const(&net->nodes, b->node);
    return n != NULL && n->generation == b->generation && net->keys_home_id == b->home &&
           zwkc_fit(net, b->node) == ZWKC_FIT_OK && highest(n) == b->security;
}

bool zwkc_claims(const zwkc_t *k, const zw_net_t *net, const zw_s2_rx_t *rx, size_t *slot)
{
    if (rx == NULL || rx->command == NULL || rx->len < 2u ||
        rx->command[0] != (uint8_t)ZW_CC_ENTRY_CONTROL || rx->command[1] != 0x01u) {
        return false; /* only an Entry Control Notification */
    }
    const size_t s = zwkc_slot_of(k, rx->node);
    if (s >= ZWKC_SLOTS || !zwkc_live(k, s, net) ||
        (uint8_t)rx->security != k->slot[s].binding.security) {
        return false; /* not an assigned keypad, or not at its class: not the table's */
    }
    if (slot != NULL) {
        *slot = s;
    }
    return true;
}

zwk_result_t zwkc_run(zwkc_t *k, size_t slot, uint32_t home, uint16_t generation,
                      const zw_s2_rx_t *rx, uint64_t now_ms, zwk_reply_t *reply)
{
    if (reply != NULL) {
        memset(reply, 0, sizeof(*reply));
        reply->result = ALARM_ERR_UNAVAILABLE;
    }
    if (slot >= ZWKC_SLOTS || k->slot[slot].binding.partition == 0u) {
        return ZWK_UNTRUSTED; /* turned off since it was claimed */
    }
    return zwk_receive(&k->slot[slot].adapter, home, generation, rx, now_ms, reply);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
}

size_t zwkc_encode(const zwkc_t *k, uint8_t *out, size_t cap)
{
    if (k == NULL || out == NULL || cap < ZWKC_BLOB_SIZE) {
        return 0u;
    }
    memset(out, 0, ZWKC_BLOB_SIZE);
    out[0] = 'K';
    out[1] = 'C';
    out[2] = 1u;
    out[3] = ZWKC_SLOTS;
    for (size_t i = 0; i < ZWKC_SLOTS; i++) {
        const zwkc_binding_t *b = &k->slot[i].binding;
        uint8_t *p = &out[4u + 12u * i];
        if (b->partition == 0u) {
            continue;
        }
        for (unsigned j = 0; j < 4u; j++) {
            p[j] = (uint8_t)((b->home >> (8u * j)) & 0xFFu);
        }
        put16(&p[4], b->node);
        put16(&p[6], b->generation);
        p[8] = b->endpoint;
        p[9] = b->partition;
        p[10] = b->security;
        p[11] = b->panic_mask;
    }
    const uint16_t crc = zw_crc16(ZW_CRC16_INIT, out, ZWKC_BLOB_SIZE - 2u);
    put16(&out[ZWKC_BLOB_SIZE - 2u], crc);
    return ZWKC_BLOB_SIZE;
}

bool zwkc_decode(zwkc_t *k, const uint8_t *p, size_t len)
{
    const zwk_port_t port = k->port;
    zwkc_init(k, &port); /* damaged assignments never grant control */
    if (p == NULL || len != ZWKC_BLOB_SIZE || p[0] != 'K' || p[1] != 'C' || p[2] != 1u ||
        p[3] != ZWKC_SLOTS || zw_crc16(ZW_CRC16_INIT, p, len - 2u) != get16(&p[len - 2u])) {
        return false;
    }
    zwkc_t in;
    zwkc_init(&in, &port);
    for (size_t i = 0; i < ZWKC_SLOTS; i++) {
        const uint8_t *q = &p[4u + 12u * i];
        zwkc_binding_t *b = &in.slot[i].binding;
        for (unsigned j = 0; j < 4u; j++) {
            b->home |= (uint32_t)q[j] << (8u * j);
        }
        b->node = get16(&q[4]);
        b->generation = get16(&q[6]);
        b->endpoint = q[8];
        b->partition = q[9];
        b->security = q[10];
        b->panic_mask = q[11];
        if (b->partition == 0u) {
            if (b->home != 0u || b->node != 0u || b->generation != 0u || q[8] != 0u || q[10] != 0u ||
                q[11] != 0u) {
                return false;
            }
            continue;
        }
        if (!arm_adapter(&in, i)) {
            return false; /* a value no assignment could have */
        }
        for (size_t j = 0; j < i; j++) {
            if (in.slot[j].binding.partition != 0u && in.slot[j].binding.node == b->node) {
                return false;
            }
        }
    }
    *k = in;
    return true;
}
