#include "zwave_zones/zw_feedback.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_crc16.h"
#include <string.h>

/* Indicator profile for manufacturer/product 0346/0101/0301 and 0401.
 * Device audio is built in. IDs are protocol values, not panel voice assets. */
enum { DISARM = 2, REJECT = 9, HOME = 10, AWAY = 11, BURGLAR = 13,
       FIRE = 14, CO = 15, BYPASS = 16, ENTRY = 17, EXIT = 18, MEDICAL = 19 };

void zwf_init(zwf_t *f) { memset(f, 0, sizeof(*f)); }

static bool same(zwf_state_t a, zwf_state_t b)
{
    /* Snapshot countdown rounding can move the deadline by a millisecond. */
    const uint64_t delta = a.end_ms > b.end_ms ? a.end_ms - b.end_ms : b.end_ms - a.end_ms;
    return a.indicator == b.indicator && delta < 1000u;
}

void zwf_view(const alarm_snapshot_t *s, zwf_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->partitions = s->partitions;
    out->taken_ms = s->taken_ms;
    out->last_event_seq = s->last_event_seq;
    for (size_t i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_status_t *p = &s->part[i];
        zwf_state_t *d = &out->part[i];
        /* Effective audible status respects silent panic/duress and silencing/bell timeout.
         * Alarm memory or `triggered` alone must never start a device siren. */
        if (p->sounder == ALARM_SOUND_FIRE) { d->indicator = FIRE; }
        else if (p->sounder == ALARM_SOUND_CO) { d->indicator = CO; }
        else if (p->sounder == ALARM_SOUND_ALARM) { d->indicator = BURGLAR; }
        else if (p->sounder == ALARM_SOUND_AUX) { d->indicator = MEDICAL; }
        else if (p->state == ALARM_STATE_ENTRY_DELAY ||
                 (p->state == ALARM_STATE_EXIT_DELAY && (p->arm_flags & ALARM_ARM_SILENT_EXIT) == 0u)) {
            d->indicator = p->state == ALARM_STATE_ENTRY_DELAY ? ENTRY : EXIT;
            d->end_ms = s->taken_ms + p->delay_remaining_ms;
        } else if (p->state == ALARM_STATE_DISARMED || p->state == ALARM_STATE_EXIT_DELAY) {
            d->indicator = DISARM;
        } else { d->indicator = p->level == ALARM_LEVEL_AWAY ? AWAY : HOME; }
    }
}

void zwf_snapshot(zwf_t *f, const zwf_view_t *view)
{
    if (f->have_view && view->taken_ms < f->view.taken_ms) { return; }
    f->view = *view;
    f->have_view = true;
}

void zwf_event(zwf_t *f, const alarm_event_t *ev)
{
    if (ev->seq <= f->last_event) { return; }
    f->last_event = ev->seq;
    if ((ev->flags & ALARM_EVF_SILENT) != 0u || ev->partition == 0u ||
        (f->have_view && ev->seq < f->view.last_event_seq)) { return; }
    uint8_t prompt = 0u;
    if (ev->type == ALARM_EV_ARM_FAILED) {
        prompt = ev->arg == ALARM_ERR_NOT_READY ? BYPASS : REJECT;
    } else if (ev->type == ALARM_EV_INVALID_CODE || ev->type == ALARM_EV_LOCKOUT) {
        prompt = REJECT;
    }
    if (prompt == 0u) { return; }
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        zwf_target_t *t = &f->target[i];
        if (t->binding.partition != ev->partition) { continue; }
        t->prompt = prompt;
        t->prompt_until = ev->mono_ms + 5000u;
        t->prompt_seq = ev->seq;
    }
}

static const zw_node_record_t *eligible(const zw_net_t *net, const zwf_binding_t *b)
{
    if (!net->checked || net->radio != ZW_NET_RADIO_OURS || net->keys_home_id != b->home) { return NULL; }
    const zw_node_record_t *n = zw_node_find_const(&net->nodes, b->node);
    if (n == NULL || n->generation != b->generation || !n->has_identity ||
        n->manufacturer_id != 0x0346u || n->product_type != 0x0101u ||
        (n->product_id != 0x0301u && n->product_id != 0x0401u) ||
        !n->has_protocol || (!n->is_listening && !n->is_flirs) ||
        n->security_classes == ZW_NODE_SECURITY_UNKNOWN ||
        (n->security_classes & (ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)) == 0u ||
        n->interview_step != ZW_IV_STEP_DONE) { return NULL; }
    for (size_t i = 0; i < n->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (n->ccs[i].cc == ZW_CC_INDICATOR && n->ccs[i].secure &&
            n->ccs[i].version >= 3u) { return n; }
    }
    return NULL;
}

zwf_fit_t zwf_fit(const zw_net_t *net, uint16_t node)
{
    const zw_node_record_t *n = zw_node_find_const(&net->nodes, node);
    if (n == NULL || !n->has_identity || n->manufacturer_id != 0x0346u ||
        n->product_type != 0x0101u || (n->product_id != 0x0301u && n->product_id != 0x0401u)) {
        return ZWF_FIT_NONE; /* no feedback profile for it: nothing to offer */
    }
    if (n->interview_step != ZW_IV_STEP_DONE || !n->has_protocol ||
        n->security_classes == ZW_NODE_SECURITY_UNKNOWN) {
        return ZWF_FIT_NOT_READY;
    }
    if ((n->security_classes & (ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)) == 0u) {
        return ZWF_FIT_NOT_SECURE;
    }
    const zwf_binding_t b = {net->keys_home_id, node, n->generation, 1u};
    return eligible(net, &b) != NULL ? ZWF_FIT_OK : ZWF_FIT_UNSUPPORTED;
}

size_t zwf_slot_of(const zwf_t *f, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZWF_TARGETS; i++) {
        if (f->target[i].binding.partition != 0u && f->target[i].binding.node == node) {
            return i;
        }
    }
    return ZWF_TARGETS;
}

size_t zwf_slot_for(const zwf_t *f, uint16_t node)
{
    const size_t own = zwf_slot_of(f, node);
    if (own < ZWF_TARGETS) {
        return own;
    }
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        if (f->target[i].binding.partition == 0u) {
            return i;
        }
    }
    return ZWF_TARGETS;
}

bool zwf_assign(zwf_t *f, size_t slot, const zw_net_t *net, uint16_t node, uint8_t partition)
{
    if (slot >= ZWF_TARGETS || partition > ALARM_MAX_PARTITIONS) { return false; }
    zwf_binding_t b = {0};
    if (partition != 0u) {
        if (node == 0u || node > 232u) { return false; }
        const zw_node_record_t *n = zw_node_find_const(&net->nodes, node);
        if (n == NULL) { return false; }
        b = (zwf_binding_t){net->keys_home_id, node, n->generation, partition};
        if (b.home == 0u || b.generation == 0u || eligible(net, &b) == NULL) { return false; }
        for (size_t i = 0; i < ZWF_TARGETS; i++) {
            if (i != slot && f->target[i].binding.node == node && f->target[i].binding.partition != 0u) {
                return false;
            }
        }
    }
    /* No accepted radio operation may outlive reassignment. */
    if (f->target[slot].inflight) { return false; }
    memset(&f->target[slot], 0, sizeof(f->target[slot]));
    f->target[slot].binding = b;
    return true;
}

size_t zwf_command(const zwf_state_t *state, uint8_t prompt, uint64_t now, uint8_t *out, size_t cap)
{
    uint8_t entries[3] = {prompt != 0u ? prompt : state->indicator, 1u, 99u};
    if (prompt != 0u) { entries[2] = 1u; }
    else if (state->indicator == ENTRY || state->indicator == EXIT) {
        if (now >= state->end_ms) { return 0u; }
        const uint64_t seconds = (state->end_ms - now + 999u) / 1000u;
        entries[1] = 7u;
        entries[2] = (uint8_t)(seconds > 255u ? 255u : seconds);
    }
    const zw_cc_indicator_set_t set = {.has_entries = true, .entries = entries, .entry_count = 1};
    return zw_cc_indicator_set_build(out, cap, &set);
}

void zwf_poll(zwf_t *f, zw_net_t *net, uint64_t now, zwf_send_fn send, void *ctx)
{
    if (!f->have_view) { return; }
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        zwf_target_t *t = &f->target[i];
        const zwf_binding_t *b = &t->binding;
        if (b->partition == 0u) { continue; }
        const zw_node_record_t *node = eligible(net, b);
        if (node == NULL || (f->view.partitions & (1u << (b->partition - 1u))) == 0u) {
            t->live = false; t->synced = false; t->prompt = 0u;
            continue;
        }
        t->live = true;
        const zwf_state_t want = f->view.part[b->partition - 1u];
        if (!same(want, t->flight)) { t->retry_at = 0u; t->failures = 0u; }
        /* Refusals must not overwrite an alarm or a subsequently successful arm. */
        if (now >= t->prompt_until || want.indicator != DISARM ||
            t->prompt_seq < f->view.last_event_seq) { t->prompt = 0u; }
        if (t->inflight || now < t->retry_at ||
            (t->synced && same(want, t->sent) && t->prompt == 0u &&
             (t->refresh_at == 0u || now < t->refresh_at))) { continue; }
        uint8_t cmd[8];
        const uint8_t prompt = t->synced && same(want, t->sent) ? t->prompt : 0u;
        const size_t len = zwf_command(&want, prompt, now, cmd, sizeof(cmd));
        if (len == 0u) { continue; } /* wait for the authoritative post-delay snapshot */
        f->stamp = (f->stamp + 1u) & 0x00FFFFFFu;
        t->tag = ZWF_TAG | f->stamp;
        t->flight = want; t->flight_prompt = prompt; t->flight_seq = t->prompt_seq;
        t->inflight = true;
        const zw_sec_class_t cls = (node->security_classes & ZW_S2_KEY_ACCESS) != 0u ? ZW_SEC_S2_ACCESS : ZW_SEC_S2_AUTH;
        const zw_s2_result_t r = send != NULL ? send(ctx, b->node, cls, cmd, len, t->tag) :
            zw_net_send(net, now, b->node, cls, cmd, len, t->tag);
        if (r != ZW_S2_OK) { (void)zwf_done(f, t->tag, r, now); }
    }
}

bool zwf_done(zwf_t *f, uint32_t tag, zw_s2_result_t result, uint64_t now)
{
    if ((tag & 0xFF000000u) != ZWF_TAG) { return false; }
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        zwf_target_t *t = &f->target[i];
        if (!t->inflight || t->tag != tag) { continue; }
        t->inflight = false;
        if (result == ZW_S2_OK) {
            t->failures = 0u; t->retry_at = 0u;
            if (t->flight_prompt == 0u) {
                t->sent = t->flight; t->synced = t->live;
                t->refresh_at = t->flight.end_ms > now && t->flight.end_ms - now > 255000u ?
                    now + 240000u : 0u;
            }
            if (t->flight_prompt != 0u && t->flight_seq == t->prompt_seq) { t->prompt = 0u; }
        } else {
            if (t->failures < 5u) { t->failures++; }
            t->retry_at = now + (t->failures < 5u ? 1000u : 30000u);
        }
    }
    return true;
}

size_t zwf_encode(const zwf_t *f, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < ZWF_BLOB_SIZE) { return 0u; }
    memset(out, 0, ZWF_BLOB_SIZE);
    out[0] = 'K'; out[1] = 'F'; out[2] = 1u; out[3] = ZWF_TARGETS;
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        const zwf_binding_t *b = &f->target[i].binding;
        uint8_t *p = out + 4u + i * 10u;
        for (unsigned j = 0; j < 4u; j++) { p[j] = (uint8_t)(b->home >> (j * 8u)); }
        p[4] = (uint8_t)b->node; p[5] = (uint8_t)(b->node >> 8);
        p[6] = (uint8_t)b->generation; p[7] = (uint8_t)(b->generation >> 8);
        p[8] = b->partition;
    }
    const uint16_t crc = zw_crc16(ZW_CRC16_INIT, out, ZWF_BLOB_SIZE - 2u);
    out[ZWF_BLOB_SIZE - 2u] = (uint8_t)crc; out[ZWF_BLOB_SIZE - 1u] = (uint8_t)(crc >> 8);
    return ZWF_BLOB_SIZE;
}

bool zwf_decode(zwf_t *f, const uint8_t *p, size_t len)
{
    zwf_init(f); /* damaged assignments never grant a destination */
    if (p == NULL || len != ZWF_BLOB_SIZE || p[0] != 'K' || p[1] != 'F' || p[2] != 1u || p[3] != ZWF_TARGETS ||
        zw_crc16(ZW_CRC16_INIT, p, len - 2u) != (uint16_t)((unsigned)p[len-2u] | (unsigned)p[len-1u] << 8)) { return false; }
    zwf_binding_t bindings[ZWF_TARGETS] = {0};
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        const uint8_t *q = p + 4u + i * 10u;
        zwf_binding_t *b = &bindings[i];
        for (unsigned j = 0; j < 4u; j++) { b->home |= (uint32_t)q[j] << (j * 8u); }
        b->node = (uint16_t)((unsigned)q[4] | (unsigned)q[5] << 8);
        b->generation = (uint16_t)((unsigned)q[6] | (unsigned)q[7] << 8);
        b->partition = q[8];
        if (q[9] != 0u || b->partition > ALARM_MAX_PARTITIONS ||
            (b->partition == 0u && (b->home != 0u || b->node != 0u || b->generation != 0u)) ||
            (b->partition != 0u && (b->home == 0u || b->node == 0u || b->node > 232u || b->generation == 0u))) { return false; }
        for (size_t j = 0; j < i; j++) {
            if (b->partition != 0u && bindings[j].partition != 0u && b->node == bindings[j].node) { return false; }
        }
    }
    for (size_t i = 0; i < ZWF_TARGETS; i++) { f->target[i].binding = bindings[i]; }
    return true;
}
