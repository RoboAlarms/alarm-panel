#include "zwave_zones/zw_keypad.h"
#include <string.h>

static void erase(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n != 0u) { *v++ = 0u; n--; }
}

bool zwk_init(zwk_t *k, const zwk_binding_t *binding, const zwk_port_t *port)
{
    if (k == NULL) { return false; }
    /* The caller may reconfigure from the current assignment/port in k itself. */
    const zwk_binding_t saved_binding = binding != NULL ? *binding : (zwk_binding_t){0};
    const zwk_port_t saved_port = port != NULL ? *port : (zwk_port_t){0};
    binding = &saved_binding;
    port = &saved_port;
    memset(k, 0, sizeof(*k));
    if (port->command == NULL || binding->home_id == 0u ||
        binding->generation == 0u || binding->node == 0u || binding->node > 232u ||
        binding->endpoint > 127u || binding->device == 0u || binding->device > ALARM_MAX_DEVICES ||
        binding->partition == 0u || binding->partition > ALARM_MAX_PARTITIONS ||
        (binding->security != ZW_SEC_S2_AUTH && binding->security != ZW_SEC_S2_ACCESS)) {
        return false;
    }
    k->binding = *binding;
    k->port = *port;
    k->enabled = true;
    return true;
}

static bool pin_copy(const zw_cc_entry_notification_t *n, char *out)
{
    if (n->data_type != ZW_ENTRY_DATA_ASCII) { return false; }
    size_t len = n->data_len;
    while (len != 0u && n->data[len - 1u] == 0xFFu) { len--; }
    if (len < ALARM_CODE_MIN_LEN || len > ALARM_CODE_MAX_LEN) { return false; }
    for (size_t i = 0u; i < len; i++) {
        if (n->data[i] < (uint8_t)'0' || n->data[i] > (uint8_t)'9') { return false; }
    }
    memcpy(out, n->data, len);
    out[len] = '\0';
    return true;
}

zwk_result_t zwk_receive(zwk_t *k, uint32_t home_id, uint16_t generation,
                         const zw_s2_rx_t *rx, uint64_t now_ms, zwk_reply_t *reply)
{
    if (reply == NULL) { return ZWK_INVALID; }
    memset(reply, 0, sizeof(*reply));
    reply->result = ALARM_ERR_UNAVAILABLE;
    if (k == NULL || rx == NULL || !k->enabled) { return ZWK_UNTRUSTED; }
    const zwk_binding_t *b = &k->binding;
    const uint8_t endpoint = rx->multi_channel ? rx->source_endpoint : 0u;
    if (home_id != b->home_id || generation != b->generation || rx->node != b->node ||
        rx->security != b->security || endpoint != b->endpoint || rx->destination_endpoint != 0u) {
        return ZWK_UNTRUSTED;
    }
    zw_cc_entry_notification_t n;
    if (!zw_cc_entry_notification_parse(rx->command, rx->len, &n)) { return ZWK_INVALID; }
    alarm_cmd_t cmd = {.partition = b->partition};
    bool needs_pin = true;
    switch (n.event_type) {
    case ZW_ENTRY_EVENT_DISARM_ALL: cmd.type = ALARM_CMD_DISARM; break;
    case ZW_ENTRY_EVENT_ARM_AWAY:
        cmd.type = ALARM_CMD_ARM; cmd.level = ALARM_LEVEL_AWAY; break;
    case ZW_ENTRY_EVENT_ARM_HOME:
        cmd.type = ALARM_CMD_ARM; cmd.level = ALARM_LEVEL_STAY; break;
    case ZW_ENTRY_EVENT_FIRE:
        needs_pin = false; cmd.type = ALARM_CMD_PANIC; cmd.panic = ALARM_PANIC_FIRE; break;
    case ZW_ENTRY_EVENT_POLICE:
        needs_pin = false; cmd.type = ALARM_CMD_PANIC; cmd.panic = ALARM_PANIC_POLICE; break;
    case ZW_ENTRY_EVENT_MEDICAL:
        needs_pin = false; cmd.type = ALARM_CMD_PANIC; cmd.panic = ALARM_PANIC_MEDICAL; break;
    default: return ZWK_IGNORED; /* cached keys/proximity/Enter are not authorization */
    }
    if (!needs_pin && ((b->panic_mask & (1u << (unsigned)cmd.panic)) == 0u || n.data_len != 0u)) {
        return ZWK_IGNORED;
    }
    if (needs_pin && !pin_copy(&n, cmd.code)) { return ZWK_INVALID; }
    for (size_t i = 0u; i < ZWK_RECENT; i++) {
        if (k->recent[i].used && now_ms - k->recent[i].handled_ms >= ZWK_RETRY_WINDOW_MS) {
            k->recent[i].used = false;
        }
        if (k->recent[i].used && k->recent[i].sequence == n.sequence) {
            *reply = k->recent[i].reply;
            erase(&cmd, sizeof(cmd));
            return ZWK_DUPLICATE;
        }
    }
    const alarm_source_t source = {.kind = ALARM_SRC_KEYPAD, .id = b->device};
    const bool done = k->port.command(k->port.ctx, &cmd, &source, reply);
    erase(&cmd, sizeof(cmd));
    if (!done) {
        memset(reply, 0, sizeof(*reply));
        reply->result = ALARM_ERR_UNAVAILABLE;
        return ZWK_BUSY;
    }
    k->recent[k->next] = (zwk_recent_t){true, n.sequence, now_ms, *reply};
    k->next = (uint8_t)((k->next + 1u) % ZWK_RECENT);
    return ZWK_HANDLED;
}
