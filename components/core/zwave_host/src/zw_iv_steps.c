/*
 * zw_iv_steps.c - the interview's steps (network.md §5's table), the lifeline (§6) and push or
 * pull: what each step asks, and what each answer leaves in the node's record. The engine
 * (zw_iv.c) sends, times and retries. See zw_iv.h.
 */
#include <string.h>

#include "zw_iv_internal.h"
#include "zwave_host/zw_s2_cc.h"
#include "zwave_proto/zw_cc.h"

#define PHASE_DONE 9u    /* the step's work is over */
#define NO_EP      0xFFu
#define MCA_NONE   0xFFu /* Multi Channel Association reported no groups */
#define PHASE_ALARM_SENSOR 3u /* step 12, after an end point's Binary Sensor: its Alarm Sensor types */

/* command-classes.md §2: the CCs the panel sends to nodes, whose versions step 6 asks
 * (Configuration too: the quirks' parameters). */
static const uint8_t USED[] = {0x20, 0x25, 0x26, 0x30, 0x31, 0x59, 0x5E, 0x60, 0x6C, 0x6F,
                               0x70, 0x71, 0x72, 0x7A, 0x80, 0x84, 0x85, 0x86, 0x87, 0x8E};

/* Step 4's probe for a node whose classes are not known, in Z-Wave JS's order (Security2CC.ts
 * interview): Unauthenticated, Authenticated, Access Control - phases 1..3. */
static const uint8_t PROBE[3] = {ZW_S2_KEY_UNAUTH, ZW_S2_KEY_AUTH, ZW_S2_KEY_ACCESS};
#define PHASE_SINGLE   5u /* a class learnt by decrypting: only it is asked (item: attempts) */
#define SINGLE_ATTEMPTS 3u /* "Try up to 3 times on the root device" (Security2CC.ts): one query
                            * each, a response timeout not resent... */
#define SINGLE_WAIT_MS  500u /* ...and 500 ms apart ("Retrying in 500ms") */

/* ---- Helpers --------------------------------------------------------------------------------- */

static bool has(const zw_node_record_t *rec, uint8_t cc)
{
    return zw_iv_has_cc(rec, cc);
}

static bool used_cc(uint8_t cc)
{
    return memchr(USED, cc, sizeof USED) != NULL;
}

static const zw_node_cc_t *entry_c(const zw_node_record_t *rec, uint8_t cc)
{
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == cc) {
            return &rec->ccs[i];
        }
    }
    return NULL;
}

static zw_node_cc_t *entry(zw_node_record_t *rec, uint8_t cc)
{
    for (size_t i = 0; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        if (rec->ccs[i].cc == cc) {
            return &rec->ccs[i];
        }
    }
    return NULL;
}

/* A CC's version: 0 when not supported, 1 when not (yet) asked. */
static uint8_t version_of(const zw_node_record_t *rec, uint8_t cc)
{
    const zw_node_cc_t *e = entry_c(rec, cc);
    if (e == NULL || !(e->supported || e->secure)) {
        return 0u;
    }
    return e->version != 0u ? (unsigned)e->version : 1u;
}

/* A sensor CC's version at an end point (0: the device itself): the device's own when it lists
 * the CC; else the version its root reported for its end points (step 8, kept without the
 * "supported" flags; 0 is "not supported" there too); 1 for an end point's CC whose version was
 * never asked (a record from before). */
static uint8_t ep_version(const zw_node_record_t *rec, uint8_t ep, uint8_t cc)
{
    const zw_node_cc_t *e = entry_c(rec, cc);
    if (ep == 0u || (e != NULL && (e->supported || e->secure))) {
        return version_of(rec, cc);
    }
    return e != NULL ? (unsigned)e->version : 1u;
}

static zw_sec_class_t class_of_bit(uint8_t bit)
{
    switch (bit) {
    case ZW_S2_KEY_ACCESS: return ZW_SEC_S2_ACCESS;
    case ZW_S2_KEY_AUTH: return ZW_SEC_S2_AUTH;
    case ZW_S2_KEY_UNAUTH: return ZW_SEC_S2_UNAUTH;
    default: return ZW_SEC_NONE;
    }
}

static bool s2_node(const zw_iv_t *iv, uint16_t node)
{
    const zw_sec_class_t c = zw_s2_highest(iv->s2, node);
    return c == ZW_SEC_S2_UNAUTH || c == ZW_SEC_S2_AUTH || c == ZW_SEC_S2_ACCESS;
}

/* A question to the node: `n` bytes already in a->cmd, at §8's class for its CC - for an end
 * point, the node's highest when it has S2 (zw_s2_class_for_endpoint). */
static void ask(const zw_iv_t *iv, const zw_iv_node_t *s, iv_ask_t *a, size_t n, uint8_t ep,
                bool answer)
{
    a->kind = IV_ASK_NODE;
    a->len = (uint8_t)(n <= ZW_IV_CMD_MAX ? n : 0u);
    a->ep = ep;
    a->cls = (uint8_t)zw_s2_class_for_endpoint(iv->s2, s->node, ep, a->cmd[0]);
    a->answer = answer;
}

static bool quirk(const zw_iv_t *iv, const zw_node_record_t *rec, zw_iv_quirk_t *q)
{
    memset(q, 0, sizeof *q);
    return iv->port.quirk != NULL && iv->port.quirk(iv->port.ctx, rec, q);
}

bool zw_iv_supervised(const zw_iv_t *iv, const zw_node_record_t *rec)
{
    zw_iv_quirk_t q;
    return has(rec, ZW_CC_SUPERVISION) && !(quirk(iv, rec, &q) && q.no_supervision);
}

/* A Set: supervised when the node lists Supervision (command-classes.md §4.14), unless its
 * quirks row says its firmware answers "fail" (zones.md §8). */
static void ask_set(const zw_iv_t *iv, const zw_iv_node_t *s, const zw_node_record_t *rec,
                    iv_ask_t *a, size_t n, uint8_t ep)
{
    ask(iv, s, a, n, ep, false);
    a->supervise = zw_iv_supervised(iv, rec);
}

static void step_done(iv_ask_t *a)
{
    a->kind = IV_STEP_DONE;
}

/* The CCs of a list, as supported, before the "controlled" mark; one-byte identifiers only. */
static size_t list_ccs(const uint8_t *list, size_t len, uint8_t *out, size_t max)
{
    zw_cc_iter_t it;
    uint16_t cc = 0u;
    bool controlled = false;
    size_t n = 0u;
    zw_cc_iter_init(&it, list, len);
    while (zw_cc_iter_next(&it, &cc, &controlled) && n < max) {
        if (!controlled && cc <= 0xFFu) {
            out[n++] = (uint8_t)cc;
        }
    }
    return n;
}

static void endpoint_add_cc(zw_node_endpoint_t *e, uint8_t cc)
{
    if (memchr(e->ccs, cc, e->cc_count) == NULL && e->cc_count < ZW_NODE_ENDPOINT_CC_MAX) {
        e->ccs[e->cc_count++] = cc;
    }
}

/* The first end point from `from` that lists `cc` (0: the device itself). */
static uint8_t ep_with(const zw_node_record_t *rec, uint8_t from, uint8_t cc)
{
    for (unsigned e = from; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        if ((e == 0u || rec->endpoints[e].used) && zw_iv_endpoint_has_cc(rec, (uint8_t)e, cc)) {
            return (uint8_t)e;
        }
    }
    return NO_EP;
}

/* A per-end-point step's cursor on the end point to ask; false when none is left. */
static bool at_endpoint(zw_iv_node_t *s, const zw_node_record_t *rec, uint8_t cc)
{
    const uint8_t e = ep_with(rec, s->ep, cc);
    if (e == NO_EP) {
        return false;
    }
    if (e != s->ep) {
        s->ep = e;
        s->phase = 0u;
        s->item = 0u;
    }
    return true;
}

static void next_endpoint(zw_iv_node_t *s)
{
    s->ep = (uint8_t)((unsigned)s->ep + 1u);
    s->phase = 0u;
    s->item = 0u;
}

/* Step 12's cursor: the next end point that lists Binary Sensor or Alarm Sensor. */
static bool at_sensor_endpoint(zw_iv_node_t *s, const zw_node_record_t *rec)
{
    const uint8_t b = ep_with(rec, s->ep, ZW_CC_BINARY_SENSOR);
    const uint8_t a = ep_with(rec, s->ep, ZW_CC_ALARM_SENSOR);
    const uint8_t e = b < a ? b : a;
    if (e == NO_EP) {
        return false;
    }
    if (e != s->ep) {
        s->ep = e;
        s->phase = 0u;
        s->item = 0u;
    }
    return true;
}

static void to_alarm_sensor(zw_iv_node_t *s)
{
    s->phase = PHASE_ALARM_SENSOR;
    s->item = 0u;
}

/* Alarm Sensor's Supported Get, heard, got no usable answer: asked, no types (field 12 empty, so a
 * finished interview does not ask it again at every start, BL-130). */
static void no_alarm_types(zw_node_endpoint_t *e)
{
    memset(e->alarm_sensor_mask, 0, sizeof e->alarm_sensor_mask);
    e->alarm_sensor_mask_len = 0u;
    e->has_alarm_sensor_mask = true;
    e->used = true;
}

/* ---- Steps 1-3: protocol info, ping, node information ---------------------------------------- */

bool zw_iv_on_protocol(zw_iv_node_t *s, zw_node_record_t *rec, const uint8_t *p, size_t len)
{
    zw_api_node_protocol_info_rsp_t r;
    if (!zw_api_node_protocol_info_rsp_parse(p, len, &r)) {
        return false;
    }
    rec->has_protocol = true;
    rec->is_listening = r.always_listening;
    rec->is_flirs = r.flirs != ZW_FLIRS_NONE;
    rec->is_routing = r.routing;
    rec->supports_long_range = s->node >= 256u;
    rec->basic_class = r.basic_device_class;
    rec->generic_class = r.generic_device_class;
    if (r.has_specific_device_class) {
        rec->specific_class = r.specific_device_class;
    }
    s->phase = 1u;
    return true;
}

void zw_iv_on_node_info(zw_iv_node_t *s, zw_node_record_t *rec, const uint8_t *ccs, size_t len)
{
    uint8_t list[ZW_NODE_CC_MAX];
    const size_t n = list_ccs(ccs, len, list, sizeof list);
    for (size_t i = 0; i < n; i++) {
        zw_iv_add_cc(rec, list[i], true, false);
    }
    s->phase = 1u;
}

static void plan_ping(const zw_iv_t *iv, const zw_iv_node_t *s, const zw_node_record_t *rec,
                      iv_ask_t *a)
{
    if (s->phase != 0u || !rec->has_protocol || !rec->is_listening) {
        step_done(a);
        return;
    }
    ask(iv, s, a, zw_cc_nop_build(a->cmd, sizeof a->cmd), 0u, false);
    a->cls = ZW_SEC_NONE;
}

/* ---- Step 4: the secure CCs, or the probe for the classes ------------------------------------ */

/* A cache-lost node whose node information lists no Security 2 (Z-Wave JS Node.ts: "If there is
 * any doubt about granted S2 security classes, we now know they are not granted" - only the
 * classes still undecided are set false). A class learnt by decrypting stays: recorded, and the
 * undecided ones refused. Nothing learnt: no security. */
static void no_security(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec)
{
    const uint8_t learnt = (uint8_t)(zw_s2_granted(iv->s2, s->node) & ZW_S2_KEYS_S2);
    rec->security_classes = learnt;
    (void)zw_s2_set_granted(iv->s2, s->node, learnt);
    s->phase = PHASE_DONE;
}

/* The probe is over: what it holds - each class that answered, and any learnt by decrypting and
 * not refused - is recorded and decides from now on. A class skipped because the panel lacks its
 * key (a Long Range node's Unauthenticated) stays undecided in the security manager, as Z-Wave JS's
 * `continue` leaves it (Security2CC.ts interview): the record cannot say "undecided", so it is
 * stored as not granted (security.md "Where the panel still differs"). */
static void probe_done(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec)
{
    const uint8_t granted = (uint8_t)(zw_s2_granted(iv->s2, s->node) & ZW_S2_KEYS_S2);
    rec->security_classes = granted;
    bool undecided = false;
    for (size_t i = 0; i < sizeof PROBE; i++) {
        undecided = undecided || ((granted & PROBE[i]) == 0u &&
                                  zw_s2_network_key(iv->s2, s->node, PROBE[i]) == NULL);
    }
    if (!undecided) {
        (void)zw_s2_set_granted(iv->s2, s->node, granted); /* every class decided */
    }
    s->phase = PHASE_DONE;
}

/* A class learnt by decrypting was asked (answered, or not after its 3 queries: Z-Wave JS "Let's
 * hope for the best"): recorded as the node's classes. The other classes stay undecided in the
 * security manager, as in Z-Wave JS; one that decrypts later is added to the record at once
 * (ZW_S2_EV_CLASS_LEARNT). The record holds no "not decided", so after a restart the classes not
 * learnt by then count as not granted (security.md "Where the panel still differs"). */
static void single_done(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec)
{
    rec->security_classes = (uint8_t)(zw_s2_granted(iv->s2, s->node) & ZW_S2_KEYS_S2);
    s->phase = PHASE_DONE;
}

/* Security 2 Commands Supported Get as Z-Wave JS's interview sends it: one query, its silence an
 * answer, a transmit failure an aborted attempt. */
static void s2_query(iv_ask_t *a)
{
    a->optional = true;
    a->tries = 1u;
    a->nok_aborts = true;
}

static bool probe_phase(uint8_t phase)
{
    return phase >= 1u && phase <= 3u;
}

/* ZW-009, ZW-010, §5 step 4 (Z-Wave JS Security2CC.ts interview): a node whose classes are known
 * is asked for its secure CCs at its highest class - up to 3 queries 500 ms apart; silence keeps
 * its classes ("Let's hope for the best"). One whose classes are not known (its cache lost): when a
 * class was learnt by decrypting (its highest is an S2 class), only that class is asked, the same
 * way; else each class is asked once in turn - Unauthenticated, Authenticated, Access Control,
 * skipping one refused or whose key the panel lacks - at that explicit class, without narrowing
 * what the node may use meanwhile (its frames at its real class still decrypt), and each one's
 * result recorded: answered = granted; silence (no answer, no Nonce Report) or "not decoded" =
 * refused. A query that was never delivered (a transmit failure) records nothing: the attempt
 * fails (Z-Wave JS's Controller_CallbackNOK). A node information frame without Security 2: no
 * security, or only the classes learnt by decrypting (no_security). */
static void plan_secure(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, iv_ask_t *a)
{
    if (s->phase >= PHASE_DONE) {
        step_done(a);
        return;
    }
    uint8_t cls;
    if (rec->security_classes == ZW_NODE_SECURITY_UNKNOWN) {
        if (!has(rec, ZW_S2_CC)) {
            no_security(iv, s, rec);
            step_done(a);
            return;
        }
        if (s->phase == 0u) {
            s->phase = s2_node(iv, s->node) ? PHASE_SINGLE : 1u;
            s->item = 0u;
        }
        if (s->phase == PHASE_SINGLE && !s2_node(iv, s->node)) {
            s->phase = 1u; /* its learnt class refused meanwhile: the whole probe */
        }
        if (s->phase == PHASE_SINGLE) {
            cls = (uint8_t)zw_s2_highest(iv->s2, s->node);
        } else {
            while (probe_phase(s->phase) &&
                   (zw_s2_network_key(iv->s2, s->node, PROBE[s->phase - 1u]) == NULL ||
                    (zw_s2_refused(iv->s2, s->node) & PROBE[s->phase - 1u]) != 0u)) {
                s->phase++;
            }
            if (!probe_phase(s->phase)) {
                probe_done(iv, s, rec);
                step_done(a);
                return;
            }
            cls = (uint8_t)class_of_bit(PROBE[s->phase - 1u]);
        }
    } else if ((rec->security_classes & ZW_S2_KEYS_S2) != 0u && s2_node(iv, s->node)) {
        cls = (uint8_t)zw_s2_highest(iv->s2, s->node);
    } else {
        step_done(a);
        return;
    }
    ask(iv, s, a, zw_s2_commands_supported_get_build(a->cmd, sizeof a->cmd), 0u, true);
    a->cls = cls;
    s2_query(a);
}

/* Z-Wave JS Security2CC.ts interview: only the first non-empty Commands Supported Report is the
 * secure CC list (hasReceivedSecureCommands); an empty one sets nothing. */
static void take_secure_list(const zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                             const uint8_t *ccs, size_t n)
{
    if (n == 0u || s->secure_taken) {
        return;
    }
    s->secure_taken = true;
    uint8_t list[ZW_NODE_CC_MAX];
    const size_t k = list_ccs(ccs, n, list, sizeof list);
    for (size_t i = 0; i < k; i++) {
        zw_iv_add_cc(rec, list[i], false, true);
    }
    (void)zw_s2_set_secure_ccs(iv->s2, s->node, ccs, n);
}

static iv_verdict_t answer_secure(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                                  const zw_s2_rx_t *rx)
{
    const uint8_t *ccs = NULL;
    size_t n = 0u;
    if (!zw_s2_commands_supported_report_parse(rx->command, rx->len, &ccs, &n)) {
        return IV_RETRY;
    }
    const bool probing = rec->security_classes == ZW_NODE_SECURITY_UNKNOWN &&
                         probe_phase(s->phase);
    /* only the highest class's answer lists the CCs (an empty list at a lower class) */
    take_secure_list(iv, s, rec, ccs, n);
    if (probing) {
        (void)zw_s2_set_class_result(iv->s2, s->node, PROBE[s->phase - 1u], true);
        s->phase++; /* the next class up: plan_secure */
        return IV_NEXT;
    }
    if (rec->security_classes == ZW_NODE_SECURITY_UNKNOWN && s->phase == PHASE_SINGLE) {
        single_done(iv, s, rec);
        return IV_NEXT;
    }
    s->phase = PHASE_DONE;
    return IV_NEXT;
}

/* ---- Steps 5-9: identity, versions, wake-up, end points, Z-Wave Plus ------------------------- */

static void plan_identity(const zw_iv_t *iv, const zw_iv_node_t *s, const zw_node_record_t *rec,
                          iv_ask_t *a)
{
    if (s->phase != 0u || !has(rec, ZW_CC_MANUFACTURER_SPECIFIC)) {
        step_done(a);
        return;
    }
    ask(iv, s, a, zw_cc_manufacturer_specific_get_build(a->cmd, sizeof a->cmd), 0u, true);
}

static iv_verdict_t answer_identity(const zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                                    const zw_s2_rx_t *rx)
{
    zw_cc_manufacturer_specific_report_t r;
    if (!zw_cc_manufacturer_specific_report_parse(rx->command, rx->len, &r)) {
        return IV_RETRY;
    }
    rec->has_identity = true;
    rec->manufacturer_id = r.manufacturer_id;
    rec->product_type = r.product_type;
    rec->product_id = r.product_id;
    zw_iv_quirk_t q;
    rec->has_quirk = quirk(iv, rec, &q);
    rec->quirk_manufacturer_id = rec->has_quirk ? (unsigned)r.manufacturer_id : 0u;
    rec->quirk_product_type = rec->has_quirk ? (unsigned)r.product_type : 0u;
    rec->quirk_product_id = rec->has_quirk ? (unsigned)r.product_id : 0u;
    s->phase = 1u;
    return IV_NEXT;
}

static void plan_versions(const zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *rec,
                          iv_ask_t *a)
{
    if (!has(rec, ZW_CC_VERSION)) {
        step_done(a);
        return;
    }
    if (s->phase == 0u) {
        ask(iv, s, a, zw_cc_version_get_build(a->cmd, sizeof a->cmd), 0u, true);
        return;
    }
    for (size_t i = s->item; i < rec->cc_count && i < ZW_NODE_CC_MAX; i++) {
        const zw_node_cc_t *e = &rec->ccs[i];
        if ((e->supported || e->secure) && used_cc(e->cc)) {
            s->item = (uint8_t)i;
            const zw_cc_version_command_class_get_t g = {e->cc};
            ask(iv, s, a, zw_cc_version_command_class_get_build(a->cmd, sizeof a->cmd, &g), 0u,
                true);
            return;
        }
    }
    step_done(a);
}

/* A CC the node is demonstrably using, whatever its Version CC Report says: Security 2 while it
 * holds an S2 class (its frames decrypt at it), S0 while it holds S0, Supervision once it sent the
 * panel a Supervision Get or answered one, Version itself (it answered through it) and
 * Manufacturer Specific once step 5 read its report. */
static bool in_use(const zw_iv_t *iv, const zw_iv_node_t *s, const zw_node_record_t *rec,
                   uint8_t cc)
{
    switch (cc) {
    case ZW_S2_CC: return s2_node(iv, s->node);
    case ZW_CC_SECURITY_0: return ((unsigned)zw_s2_granted(iv->s2, s->node) & ZW_S2_KEY_S0) != 0u;
    case ZW_CC_SUPERVISION: return s->spv_seen;
    case ZW_CC_VERSION: return true;
    case ZW_CC_MANUFACTURER_SPECIFIC: return rec->has_identity;
    default: return false;
    }
}

static iv_verdict_t answer_versions(const zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                                    const zw_s2_rx_t *rx)
{
    if (s->phase == 0u) {
        /* the device's own versions (node cache field 11: firmware updates, ZW-033); a report
         * that doesn't parse leaves them unknown, the step goes on */
        zw_cc_version_report_t v;
        if (zw_cc_version_report_parse(rx->command, rx->len, &v)) {
            rec->has_versions = true;
            rec->fw_major = v.firmware0_major;
            rec->fw_minor = v.firmware0_minor;
            rec->hw_version = v.has_v2 ? v.hardware_version : 0xFFu;
            rec->protocol_major = v.protocol_major;
            rec->protocol_minor = v.protocol_minor;
            rec->fw_target_count = 0u;
            for (uint8_t i = 0; v.has_v2 && i < v.other_target_count && i < ZW_NODE_FW_TARGETS_MAX;
                 i++) {
                rec->fw_targets[i][0] = v.other_targets[2u * i];
                rec->fw_targets[i][1] = v.other_targets[2u * i + 1u];
                rec->fw_target_count++;
            }
        }
        s->phase = 1u;
        s->item = 0u;
        return IV_NEXT;
    }
    zw_cc_version_command_class_report_t r;
    zw_node_cc_t *e = NULL;
    if (!zw_cc_version_command_class_report_parse(rx->command, rx->len, &r) ||
        (e = entry(rec, r.cc)) == NULL) {
        return IV_RETRY;
    }
    e->version = r.version;
    if (r.version == 0u && in_use(iv, s, rec, r.cc)) {
        e->version = 1u; /* its "0" is not believed: it uses the CC before the panel's eyes */
    } else if (r.version == 0u) {
        e->supported = false; /* command-classes.md §3: version 0 is "not supported" */
        e->secure = false;
    }
    s->item = (uint8_t)((unsigned)s->item + 1u);
    return IV_NEXT;
}

static void plan_wake_up(const zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *rec,
                         iv_ask_t *a)
{
    if (!has(rec, ZW_CC_WAKE_UP) || s->phase >= 3u) {
        step_done(a);
        return;
    }
    if (s->phase == 0u && version_of(rec, ZW_CC_WAKE_UP) < 2u) {
        s->phase = 1u;
    }
    if (s->phase == 0u) {
        ask(iv, s, a, zw_cc_wake_up_interval_capabilities_get_build(a->cmd, sizeof a->cmd), 0u,
            true);
        return;
    }
    if (s->phase == 1u) {
        ask(iv, s, a, zw_cc_wake_up_interval_get_build(a->cmd, sizeof a->cmd), 0u, true);
        return;
    }
    /* §7: interval 0 is left alone; else the owner's choice, with the panel as destination */
    const uint32_t now = rec->wake_up_interval_seconds;
    uint32_t want = iv->port.wake_interval != NULL ? iv->port.wake_interval(iv->port.ctx, rec)
                                                   : ZW_IV_WAKE_KEEP;
    if (want == ZW_IV_WAKE_KEEP) {
        want = now;
    }
    const uint8_t own = (uint8_t)iv->own_node; /* Wake Up names nodes in one byte */
    if (now == 0u || (want == now && rec->wake_up_notify_node_id == own)) {
        step_done(a);
        return;
    }
    const zw_cc_wake_up_interval_set_t set = {want, own};
    ask_set(iv, s, rec, a, zw_cc_wake_up_interval_set_build(a->cmd, sizeof a->cmd, &set), 0u);
}

static iv_verdict_t answer_wake_up(zw_iv_node_t *s, zw_node_record_t *rec, const zw_s2_rx_t *rx)
{
    if (s->phase == 0u) {
        zw_cc_wake_up_interval_capabilities_report_t c;
        if (!zw_cc_wake_up_interval_capabilities_report_parse(rx->command, rx->len, &c)) {
            return IV_RETRY;
        }
        rec->has_wake_up = true;
        rec->wake_up_min_seconds = c.min_seconds;
        rec->wake_up_max_seconds = c.max_seconds;
        rec->wake_up_step_seconds = c.step_seconds;
        s->phase = 1u;
        return IV_NEXT;
    }
    if (s->phase == 1u) {
        zw_cc_wake_up_interval_report_t r;
        if (!zw_cc_wake_up_interval_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        rec->has_wake_up = true;
        rec->wake_up_interval_seconds = r.interval_seconds;
        rec->wake_up_notify_node_id = r.node_id;
        s->phase = 2u;
        return IV_NEXT;
    }
    zw_cc_wake_up_interval_set_t set; /* the Set went: what it set */
    if (zw_cc_wake_up_interval_set_parse(s->cmd, s->cmd_len, &set)) {
        rec->wake_up_interval_seconds = set.interval_seconds;
        rec->wake_up_notify_node_id = set.node_id_to_notify;
    }
    s->phase = 3u;
    return IV_NEXT;
}

/* The sensor CCs whose steps (11-13) depend on their version. */
static const uint8_t EP_VERSIONED[] = {ZW_CC_NOTIFICATION, ZW_CC_BINARY_SENSOR,
                                       ZW_CC_MULTILEVEL_SENSOR};

/* Step 8's last part: a sensor CC only end points list, whose version the record lacks - the root
 * answers Version for its end points' CCs (the Multi Channel specification) - else 0. */
static uint8_t endpoint_only_cc(const zw_node_record_t *rec)
{
    if (!has(rec, ZW_CC_VERSION) || rec->cc_count >= ZW_NODE_CC_MAX) {
        return 0u;
    }
    for (uint8_t e = 1u; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        for (size_t k = 0; rec->endpoints[e].used && k < sizeof EP_VERSIONED; k++) {
            if (zw_iv_endpoint_has_cc(rec, e, EP_VERSIONED[k]) &&
                entry_c(rec, EP_VERSIONED[k]) == NULL) {
                return EP_VERSIONED[k];
            }
        }
    }
    return 0u;
}

static void plan_endpoints(const zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *rec,
                           iv_ask_t *a)
{
    if (!has(rec, ZW_CC_MULTI_CHANNEL) || s->phase >= PHASE_DONE) {
        step_done(a);
        return;
    }
    if (s->phase != 0u && s->phase != 3u && s->item > s->ep_count) {
        s->phase = 3u; /* every end point read: the versions of their own sensor CCs */
    }
    if (s->phase == 3u) {
        const uint8_t cc = endpoint_only_cc(rec);
        if (cc == 0u) {
            step_done(a);
            return;
        }
        const zw_cc_version_command_class_get_t g = {cc};
        ask(iv, s, a, zw_cc_version_command_class_get_build(a->cmd, sizeof a->cmd, &g), 0u, true);
        return;
    }
    if (s->phase == 0u) {
        ask(iv, s, a, zw_cc_multi_channel_end_point_get_build(a->cmd, sizeof a->cmd), 0u, true);
        return;
    }
    if (s->phase == 1u) {
        const zw_cc_multi_channel_capability_get_t g = {s->item};
        ask(iv, s, a, zw_cc_multi_channel_capability_get_build(a->cmd, sizeof a->cmd, &g), 0u,
            true);
        return;
    }
    /* §5 step 8: an S2 node's end point lists its secure CCs in its own Commands Supported */
    ask(iv, s, a, zw_s2_commands_supported_get_build(a->cmd, sizeof a->cmd), s->item, true);
    a->cls = (uint8_t)zw_s2_highest(iv->s2, s->node);
    s2_query(a); /* an end point is asked once (Security2CC.ts: MAX_ATTEMPTS 1) */
}

static iv_verdict_t answer_endpoints(const zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                                     const zw_s2_rx_t *rx)
{
    if (s->phase == 0u) {
        zw_cc_multi_channel_end_point_report_t r;
        if (!zw_cc_multi_channel_end_point_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        const unsigned kept = ZW_NODE_ENDPOINTS_MAX - 1u;
        s->ep_count =
            (uint8_t)((unsigned)r.individual_count < kept ? (unsigned)r.individual_count : kept);
        for (size_t e = 1; e < ZW_NODE_ENDPOINTS_MAX; e++) {
            memset(&rec->endpoints[e], 0, sizeof rec->endpoints[e]);
        }
        s->item = 1u;
        s->phase = s->ep_count > 0u ? 1u : PHASE_DONE;
        return IV_NEXT;
    }
    if (s->phase == 3u) {
        /* the version of an end point's own sensor CC, from the root: kept in the device's list
         * with neither flag - the device itself does not have it (zw_iv_has_cc stays false) */
        zw_cc_version_command_class_report_t r;
        if (!zw_cc_version_command_class_report_parse(rx->command, rx->len, &r) ||
            entry(rec, r.cc) != NULL || rec->cc_count >= ZW_NODE_CC_MAX) {
            return IV_RETRY;
        }
        zw_node_cc_t *c = &rec->ccs[rec->cc_count++];
        memset(c, 0, sizeof *c);
        c->cc = r.cc;
        c->version = r.version;
        return IV_NEXT;
    }
    if (s->item >= ZW_NODE_ENDPOINTS_MAX) {
        return IV_RETRY; /* never: the plan asks no end point past the kept ones */
    }
    zw_node_endpoint_t *e = &rec->endpoints[s->item];
    if (s->phase == 1u) {
        zw_cc_multi_channel_capability_report_t r;
        if (!zw_cc_multi_channel_capability_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        uint8_t list[ZW_NODE_ENDPOINT_CC_MAX];
        const size_t n = list_ccs(r.cc_list, r.cc_list_len, list, sizeof list);
        e->used = true;
        e->has_class = true;
        e->generic_class = r.generic_class;
        e->specific_class = r.specific_class;
        e->cc_count = 0u;
        for (size_t i = 0; i < n; i++) {
            endpoint_add_cc(e, list[i]);
        }
        if (s2_node(iv, s->node)) {
            s->phase = 2u;
        } else {
            s->item = (uint8_t)((unsigned)s->item + 1u);
        }
        return IV_NEXT;
    }
    const uint8_t *ccs = NULL;
    size_t n = 0u;
    if (!zw_s2_commands_supported_report_parse(rx->command, rx->len, &ccs, &n)) {
        return IV_RETRY;
    }
    uint8_t list[ZW_NODE_ENDPOINT_CC_MAX];
    const size_t k = list_ccs(ccs, n, list, sizeof list);
    for (size_t i = 0; i < k; i++) {
        endpoint_add_cc(e, list[i]);
    }
    s->phase = 1u;
    s->item = (uint8_t)((unsigned)s->item + 1u);
    return IV_NEXT;
}

static void plan_zwave_plus(const zw_iv_t *iv, const zw_iv_node_t *s,
                            const zw_node_record_t *rec, iv_ask_t *a)
{
    if (s->phase != 0u || !has(rec, ZW_CC_ZWAVE_PLUS_INFO)) {
        step_done(a);
        return;
    }
    ask(iv, s, a, zw_cc_zwave_plus_info_get_build(a->cmd, sizeof a->cmd), 0u, true);
}

static iv_verdict_t answer_zwave_plus(zw_iv_node_t *s, zw_node_record_t *rec,
                                      const zw_s2_rx_t *rx)
{
    zw_cc_zwave_plus_info_report_t r;
    if (!zw_cc_zwave_plus_info_report_parse(rx->command, rx->len, &r)) {
        return IV_RETRY;
    }
    rec->has_zwave_plus = true;
    rec->zwave_plus_role_type = r.role_type;
    rec->zwave_plus_node_type = r.node_type;
    rec->installer_icon = r.installer_icon;
    rec->user_icon = r.user_icon;
    s->phase = 1u;
    return IV_NEXT;
}

/* ---- Step 10: the lifeline (§6) -------------------------------------------------------------- */

/* §6's first case: end points, none of which associates by itself, and Multi Channel
 * Association on the device. */
static bool mc_device(const zw_node_record_t *rec)
{
    bool endpoints = false;
    for (uint8_t e = 1u; e < ZW_NODE_ENDPOINTS_MAX; e++) {
        if (!rec->endpoints[e].used) {
            continue;
        }
        endpoints = true;
        if (zw_iv_endpoint_has_cc(rec, e, ZW_CC_ASSOCIATION) ||
            zw_iv_endpoint_has_cc(rec, e, ZW_CC_MULTI_CHANNEL_ASSOCIATION)) {
            return false;
        }
    }
    return endpoints && has(rec, ZW_CC_MULTI_CHANNEL) && has(rec, ZW_CC_MULTI_CHANNEL_ASSOCIATION);
}

/* ...and the group within its Multi Channel Association groups. */
static bool use_mc(const zw_iv_node_t *s, const zw_node_record_t *rec)
{
    return mc_device(rec) && s->mca_groups != 0u && s->mca_groups != MCA_NONE &&
           s->mca_groups >= rec->lifeline_group;
}

/* Through Multi Channel Association: the §6 Set, or a device without plain Association. */
static bool via_mca(const zw_iv_node_t *s, const zw_node_record_t *rec)
{
    return use_mc(s, rec) || !has(rec, ZW_CC_ASSOCIATION);
}

static void no_lifeline(zw_iv_node_t *s, zw_node_record_t *rec)
{
    rec->has_lifeline = true; /* decided: "Does not report to the panel" */
    rec->lifeline_group = 0u;
    rec->lifeline_set = false;
    s->phase = PHASE_DONE;
}

static void plan_lifeline(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, iv_ask_t *a)
{
    for (unsigned guard = 0; guard < 8u; guard++) {
        switch (s->phase) {
        case 0u:
            if (!has(rec, ZW_CC_ASSOCIATION) && !has(rec, ZW_CC_MULTI_CHANNEL_ASSOCIATION)) {
                no_lifeline(s, rec);
            } else if (has(rec, ZW_CC_ZWAVE_PLUS_INFO)) {
                rec->lifeline_group = ZW_ASSOCIATION_LIFELINE_GROUP;
                s->phase = 2u;
            } else if (has(rec, ZW_CC_ASSOCIATION_GROUP_INFORMATION)) {
                const zw_cc_agi_info_get_t g = {false, false, ZW_ASSOCIATION_LIFELINE_GROUP};
                ask(iv, s, a, zw_cc_agi_info_get_build(a->cmd, sizeof a->cmd, &g), 0u, true);
                return;
            } else {
                s->phase = 1u;
            }
            break;
        case 1u: {
            zw_iv_quirk_t q;
            if (quirk(iv, rec, &q) && q.lifeline_group != 0u) {
                rec->lifeline_group = q.lifeline_group;
                s->phase = 2u;
            } else {
                no_lifeline(s, rec);
            }
            break;
        }
        case 2u:
            if (mc_device(rec) && s->mca_groups == 0u) {
                ask(iv, s, a,
                    zw_cc_mc_association_supported_groupings_get_build(a->cmd, sizeof a->cmd), 0u,
                    true);
                return;
            }
            s->phase = 3u;
            break;
        case 3u: {
            const uint8_t own = (uint8_t)iv->own_node;
            size_t n;
            if (use_mc(s, rec)) {
                const uint8_t dest[3] = {0x00u, own, 0x00u}; /* marker, node 1, endpoint 0 */
                const zw_cc_mc_association_set_t set = {rec->lifeline_group, dest, sizeof dest};
                n = zw_cc_mc_association_set_build(a->cmd, sizeof a->cmd, &set);
            } else if (has(rec, ZW_CC_ASSOCIATION)) {
                const zw_cc_association_set_t set = {rec->lifeline_group, &own, 1u};
                n = zw_cc_association_set_build(a->cmd, sizeof a->cmd, &set);
            } else {
                const zw_cc_mc_association_set_t set = {rec->lifeline_group, &own, 1u};
                n = zw_cc_mc_association_set_build(a->cmd, sizeof a->cmd, &set);
            }
            ask_set(iv, s, rec, a, n, 0u);
            return;
        }
        case 4u: {
            size_t n;
            if (via_mca(s, rec)) {
                const zw_cc_mc_association_get_t g = {rec->lifeline_group};
                n = zw_cc_mc_association_get_build(a->cmd, sizeof a->cmd, &g);
            } else {
                const zw_cc_association_get_t g = {rec->lifeline_group};
                n = zw_cc_association_get_build(a->cmd, sizeof a->cmd, &g);
            }
            ask(iv, s, a, n, 0u, true);
            return;
        }
        default:
            step_done(a);
            return;
        }
    }
    step_done(a);
}

/* The read-back: the panel among the group's destinations, as it was set. *more: this Report is a
 * part, more follow (its "reports to follow"). */
static bool lifeline_reads_back(const zw_iv_t *iv, const zw_iv_node_t *s,
                                const zw_node_record_t *rec, const zw_s2_rx_t *rx, bool *more)
{
    *more = false;
    if (!via_mca(s, rec)) {
        zw_cc_association_report_t r;
        if (!zw_cc_association_report_parse(rx->command, rx->len, &r)) {
            return false;
        }
        *more = r.reports_to_follow != 0u;
        return r.node_ids != NULL &&
               memchr(r.node_ids, (int)(uint8_t)iv->own_node, r.node_id_count) != NULL;
    }
    zw_cc_mc_association_report_t r;
    if (!zw_cc_mc_association_report_parse(rx->command, rx->len, &r)) {
        return false;
    }
    *more = r.reports_to_follow != 0u;
    const bool endpoint = use_mc(s, rec);
    zw_cc_mc_dest_iter_t it;
    zw_cc_mc_destination_t d;
    zw_cc_mc_dest_iter_init(&it, r.destinations, r.destinations_len);
    while (zw_cc_mc_dest_iter_next(&it, &d)) {
        if (d.node_id == iv->own_node && d.is_endpoint == endpoint &&
            (!endpoint || (!d.is_mask && d.endpoint == 0u))) {
            return true;
        }
    }
    return false;
}

static iv_verdict_t answer_lifeline(const zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                                    const zw_s2_rx_t *rx)
{
    switch (s->phase) {
    case 0u: { /* AGI Info: group 1 with the Lifeline profile */
        zw_cc_agi_info_report_t r;
        zw_cc_agi_group_info_t g;
        if (!zw_cc_agi_info_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        const bool lifeline = zw_cc_agi_group_info_get(r.groups, r.groups_len, 0u, &g) &&
                              g.group == ZW_ASSOCIATION_LIFELINE_GROUP &&
                              g.profile == ZW_AGI_PROFILE_GENERAL_LIFELINE;
        if (lifeline) {
            rec->lifeline_group = ZW_ASSOCIATION_LIFELINE_GROUP;
        }
        s->phase = lifeline ? 2u : 1u;
        return IV_NEXT;
    }
    case 2u: {
        zw_cc_mc_association_supported_groupings_report_t r;
        if (!zw_cc_mc_association_supported_groupings_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        s->mca_groups = r.group_count != 0u ? (unsigned)r.group_count : MCA_NONE;
        s->phase = 3u;
        return IV_NEXT;
    }
    case 3u:
        s->phase = 4u; /* the Set went: read it back, on the same tries */
        return IV_CONTINUE;
    default: {
        bool more = false;
        if (!lifeline_reads_back(iv, s, rec, rx, &more)) {
            if (more) {
                return IV_MORE; /* a part without the panel: the next part is awaited */
            }
            s->phase = 3u; /* §6: set again */
            return IV_RETRY;
        }
        rec->has_lifeline = true;
        rec->lifeline_set = true;
        s->phase = PHASE_DONE;
        return IV_NEXT;
    }
    }
}

/* ---- Steps 11-15: notification, binary and alarm sensor, multilevel sensor, battery, quirks -- */

static void set_mode(zw_node_endpoint_t *e, uint8_t mode)
{
    e->has_push_pull = true;
    e->push_pull_mode = mode;
}

/* The lifeline's AGI command list names Notification Report. */
static bool lists_notification_report(const zw_cc_agi_command_list_report_t *r)
{
    const uint8_t *p = r->cc_command_pairs;
    size_t i = 0u;
    while (p != NULL && i + 1u < r->cc_command_pairs_len) {
        if (p[i] >= 0xF1u) { /* a two-byte CC, then its command */
            i += 3u;
            continue;
        }
        if (p[i] == ZW_CC_NOTIFICATION && p[i + 1u] == ZW_CMD_NOTIFICATION_REPORT) {
            return true;
        }
        i += 2u;
    }
    return false;
}

static void plan_notification(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, iv_ask_t *a)
{
    for (unsigned guard = 0; guard < 64u; guard++) {
        if (!at_endpoint(s, rec, ZW_CC_NOTIFICATION)) {
            step_done(a);
            return;
        }
        /* the version at this end point: an end point may list Notification its root does not */
        const uint8_t v = ep_version(rec, s->ep, ZW_CC_NOTIFICATION);
        if (v < 2u) {
            next_endpoint(s); /* v1 has no Supported Get: nothing to ask here */
            continue;
        }
        zw_node_endpoint_t *e = &rec->endpoints[s->ep];
        const uint8_t count = e->notification_count;
        switch (s->phase) {
        case 0u:
            ask(iv, s, a, zw_cc_notification_supported_get_build(a->cmd, sizeof a->cmd), s->ep,
                true);
            return;
        case 1u:
            if (v < 3u || s->item >= count) {
                s->phase = 2u;
                s->item = 0u;
                break;
            }
            {
                const zw_cc_notification_event_supported_get_t g = {
                    e->notifications[s->item].notification_type};
                ask(iv, s, a,
                    zw_cc_notification_event_supported_get_build(a->cmd, sizeof a->cmd, &g),
                    s->ep, true);
            }
            return;
        case 2u: /* push or pull */
            if (count == 0u) {
                next_endpoint(s);
                break;
            }
            if (!has(rec, ZW_CC_ASSOCIATION) && !has(rec, ZW_CC_MULTI_CHANNEL_ASSOCIATION)) {
                set_mode(e, ZW_NODE_MODE_PULL);
                s->phase = 3u;
                break;
            }
            if (s->agi == 0u && has(rec, ZW_CC_ASSOCIATION_GROUP_INFORMATION) &&
                rec->lifeline_group != 0u) {
                const zw_cc_agi_command_list_get_t g = {0x00u, rec->lifeline_group};
                ask(iv, s, a, zw_cc_agi_command_list_get_build(a->cmd, sizeof a->cmd, &g), 0u,
                    true);
                return;
            }
            if (s->agi == 1u) {
                set_mode(e, ZW_NODE_MODE_PUSH);
                s->phase = 3u;
                break;
            }
            if (s->item == 0u) { /* enable one type, then read it */
                const zw_cc_notification_set_t set = {e->notifications[0].notification_type,
                                                      ZW_NOTIFICATION_STATUS_ON};
                ask_set(iv, s, rec, a, zw_cc_notification_set_build(a->cmd, sizeof a->cmd, &set),
                        s->ep);
                return;
            }
            {
                const zw_cc_notification_get_t g = {0x00u, e->notifications[0].notification_type,
                                                    true, 0x00u};
                ask(iv, s, a, zw_cc_notification_get_build(a->cmd, sizeof a->cmd, &g), s->ep,
                    true);
                a->optional = true; /* §5: nothing answers - assume push */
            }
            return;
        case 3u: /* the current state of each type */
            if (s->item >= count) {
                s->phase = 4u;
                s->item = 0u;
                break;
            }
            {
                const zw_cc_notification_get_t g = {
                    0x00u, e->notifications[s->item].notification_type, true, 0x00u};
                ask(iv, s, a, zw_cc_notification_get_build(a->cmd, sizeof a->cmd, &g), s->ep,
                    true);
            }
            return;
        default: /* push devices: Notification Set type FF, each type */
            if (!e->has_push_pull || e->push_pull_mode != ZW_NODE_MODE_PUSH || s->item >= count) {
                next_endpoint(s);
                break;
            }
            {
                const zw_cc_notification_set_t set = {e->notifications[s->item].notification_type,
                                                      ZW_NOTIFICATION_STATUS_ON};
                ask_set(iv, s, rec, a, zw_cc_notification_set_build(a->cmd, sizeof a->cmd, &set),
                        s->ep);
            }
            return;
        }
    }
    step_done(a);
}

static iv_verdict_t answer_notification(zw_iv_node_t *s, zw_node_record_t *rec,
                                        const zw_s2_rx_t *rx)
{
    zw_node_endpoint_t *e = &rec->endpoints[s->ep];
    if (s->phase == 0u) {
        zw_cc_notification_supported_report_t r;
        if (!zw_cc_notification_supported_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        uint8_t n = 0u;
        for (unsigned t = 1u; t < r.type_mask_len * 8u && t <= 0xFFu; t++) {
            if (n < ZW_NODE_NOTIFICATION_MAX && zw_cc_notification_type_supported(r.type_mask,
                                                                                 r.type_mask_len,
                                                                                 (uint8_t)t)) {
                memset(&e->notifications[n], 0, sizeof e->notifications[n]);
                e->notifications[n].notification_type = (uint8_t)t;
                n++;
            }
        }
        e->used = true;
        e->notification_count = n;
        s->phase = 1u;
        s->item = 0u;
        return IV_NEXT;
    }
    if (s->phase == 1u) {
        zw_cc_notification_event_supported_report_t r;
        if (!zw_cc_notification_event_supported_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        zw_node_notification_t *nt = &e->notifications[s->item];
        const size_t m = r.event_mask_len < ZW_NODE_MASK_BYTES ? r.event_mask_len
                                                               : ZW_NODE_MASK_BYTES;
        memset(nt->event_mask, 0, sizeof nt->event_mask);
        if (m > 0u) {
            memcpy(nt->event_mask, r.event_mask, m);
        }
        nt->event_mask_len = (uint8_t)m;
        s->item = (uint8_t)((unsigned)s->item + 1u);
        return IV_NEXT;
    }
    if (s->phase == 2u) {
        if (s->cmd[0] == ZW_CC_ASSOCIATION_GROUP_INFORMATION) {
            zw_cc_agi_command_list_report_t r;
            if (!zw_cc_agi_command_list_report_parse(rx->command, rx->len, &r)) {
                return IV_RETRY;
            }
            s->agi = lists_notification_report(&r) ? 1u : 2u;
            return IV_NEXT;
        }
        if (s->item == 0u) {
            s->item = 1u; /* enabled: now read it */
            return IV_NEXT;
        }
        zw_cc_notification_report_t r;
        if (!zw_cc_notification_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        const bool pull = !r.is_v1 && (r.status == ZW_NOTIFICATION_STATUS_OFF ||
                                       r.status == ZW_NOTIFICATION_STATUS_NO_PENDING);
        set_mode(e, (uint8_t)(pull ? ZW_NODE_MODE_PULL : ZW_NODE_MODE_PUSH));
        s->phase = 3u;
        s->item = 0u;
        return IV_NEXT;
    }
    s->item = (uint8_t)((unsigned)s->item + 1u); /* a state read, or a type enabled */
    return IV_NEXT;
}

/* Step 12, per end point: Binary Sensor (phases 0-2), then Alarm Sensor's Supported Get
 * (PHASE_ALARM_SENSOR, BL-085) - folded into this step, not a step of its own: field 0E keeps
 * step numbers, and a new one would renumber the steps after it. A device that does not answer
 * it is not stalled: the question is optional, and silence leaves no types (zw_iv_on_silence).
 * A finished interview that lacks the types (s->alarm_only, BL-130) asks only the end points
 * without them: no Binary Sensor question. */
static void plan_binary(const zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *rec,
                        iv_ask_t *a)
{
    for (unsigned guard = 0; guard < 64u; guard++) {
        if (!at_sensor_endpoint(s, rec)) {
            step_done(a);
            return;
        }
        if (s->phase < PHASE_ALARM_SENSOR &&
            (s->alarm_only || !zw_iv_endpoint_has_cc(rec, s->ep, ZW_CC_BINARY_SENSOR))) {
            to_alarm_sensor(s);
        }
        if (s->phase == PHASE_ALARM_SENSOR) {
            if (!zw_iv_endpoint_has_cc(rec, s->ep, ZW_CC_ALARM_SENSOR) ||
                (s->alarm_only && rec->endpoints[s->ep].has_alarm_sensor_mask)) {
                next_endpoint(s);
                continue;
            }
            ask(iv, s, a, zw_cc_alarm_sensor_supported_get_build(a->cmd, sizeof a->cmd), s->ep,
                true);
            a->optional = true;
            return;
        }
        const uint8_t v = ep_version(rec, s->ep, ZW_CC_BINARY_SENSOR);
        const zw_node_endpoint_t *e = &rec->endpoints[s->ep];
        if (s->phase == 0u && v < 2u) {
            s->phase = 2u;
        }
        if (s->phase == 0u) {
            ask(iv, s, a, zw_cc_binary_sensor_supported_get_build(a->cmd, sizeof a->cmd), s->ep,
                true);
            return;
        }
        if (s->phase == 2u) {
            const zw_cc_binary_sensor_get_t g = {false, 0u};
            ask(iv, s, a, zw_cc_binary_sensor_get_build(a->cmd, sizeof a->cmd, &g), s->ep, true);
            return;
        }
        for (unsigned t = (unsigned)s->item; t < (unsigned)e->binary_sensor_mask_len * 8u; t++) {
            if (t > 0u && zw_cc_binary_sensor_type_supported(e->binary_sensor_mask,
                                                             e->binary_sensor_mask_len,
                                                             (uint8_t)t)) {
                s->item = (uint8_t)t;
                const zw_cc_binary_sensor_get_t g = {true, (uint8_t)t};
                ask(iv, s, a, zw_cc_binary_sensor_get_build(a->cmd, sizeof a->cmd, &g), s->ep,
                    true);
                return;
            }
        }
        to_alarm_sensor(s);
    }
    step_done(a);
}

static iv_verdict_t answer_binary(zw_iv_node_t *s, zw_node_record_t *rec, const zw_s2_rx_t *rx)
{
    zw_node_endpoint_t *e = &rec->endpoints[s->ep];
    if (s->phase == PHASE_ALARM_SENSOR) {
        zw_cc_alarm_sensor_supported_report_t r;
        if (!zw_cc_alarm_sensor_supported_report_parse(rx->command, rx->len, &r)) {
            /* A report that doesn't parse is taken as silence, not tried again: the question is
             * optional, and a device that always answers it badly must not stall its interview
             * and lose its other inputs (no types known, as zw_iv_on_silence). */
            no_alarm_types(e);
            s->alarm_stored = true;
            next_endpoint(s);
            return IV_NEXT;
        }
        /* types past what the cache holds are dropped: the zones use 1, 2 and 4 */
        const size_t m = r.mask_len < ZW_NODE_MASK_BYTES ? r.mask_len : ZW_NODE_MASK_BYTES;
        memset(e->alarm_sensor_mask, 0, sizeof e->alarm_sensor_mask);
        if (m > 0u) {
            memcpy(e->alarm_sensor_mask, r.mask, m);
        }
        e->alarm_sensor_mask_len = (uint8_t)m;
        e->has_alarm_sensor_mask = true;
        e->used = true;
        s->alarm_stored = true;
        next_endpoint(s);
        return IV_NEXT;
    }
    if (s->phase == 0u) {
        zw_cc_binary_sensor_supported_report_t r;
        if (!zw_cc_binary_sensor_supported_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        const size_t m = r.mask_len < ZW_NODE_MASK_BYTES ? r.mask_len : ZW_NODE_MASK_BYTES;
        memset(e->binary_sensor_mask, 0, sizeof e->binary_sensor_mask);
        if (m > 0u) {
            memcpy(e->binary_sensor_mask, r.mask, m);
        }
        e->binary_sensor_mask_len = (uint8_t)m;
        e->has_binary_sensor_mask = true;
        e->used = true;
        s->phase = 1u;
        s->item = 1u;
        return IV_NEXT;
    }
    if (s->phase == 1u) {
        s->item = (uint8_t)((unsigned)s->item + 1u);
        return IV_NEXT;
    }
    to_alarm_sensor(s);
    return IV_NEXT;
}

static void plan_multilevel(const zw_iv_t *iv, zw_iv_node_t *s, const zw_node_record_t *rec,
                            iv_ask_t *a)
{
    for (unsigned guard = 0; guard < 16u; guard++) {
        if (!at_endpoint(s, rec, ZW_CC_MULTILEVEL_SENSOR)) {
            step_done(a);
            return;
        }
        const uint8_t v = ep_version(rec, s->ep, ZW_CC_MULTILEVEL_SENSOR);
        const zw_node_endpoint_t *e = &rec->endpoints[s->ep];
        if (s->phase == 0u && v < 5u) {
            s->phase = 2u;
        }
        if (s->phase == 0u) {
            ask(iv, s, a,
                zw_cc_multilevel_sensor_supported_sensor_get_build(a->cmd, sizeof a->cmd), s->ep,
                true);
            return;
        }
        if (s->phase == 1u &&
            zw_cc_multilevel_sensor_type_supported(e->multilevel_mask, e->multilevel_mask_len,
                                                   ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE)) {
            const zw_cc_multilevel_sensor_get_t g = {true,
                                                     ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE,
                                                     ZW_MULTILEVEL_SENSOR_SCALE_CELSIUS};
            ask(iv, s, a, zw_cc_multilevel_sensor_get_build(a->cmd, sizeof a->cmd, &g), s->ep,
                true);
            return;
        }
        if (s->phase == 2u) {
            const zw_cc_multilevel_sensor_get_t g = {false, 0u, 0u};
            ask(iv, s, a, zw_cc_multilevel_sensor_get_build(a->cmd, sizeof a->cmd, &g), s->ep,
                true);
            return;
        }
        next_endpoint(s); /* no air temperature here */
    }
    step_done(a);
}

static iv_verdict_t answer_multilevel(zw_iv_node_t *s, zw_node_record_t *rec,
                                      const zw_s2_rx_t *rx)
{
    if (s->phase == 0u) {
        zw_node_endpoint_t *e = &rec->endpoints[s->ep];
        zw_cc_multilevel_sensor_supported_sensor_report_t r;
        if (!zw_cc_multilevel_sensor_supported_sensor_report_parse(rx->command, rx->len, &r)) {
            return IV_RETRY;
        }
        const size_t m = r.mask_len < ZW_NODE_MASK_BYTES ? r.mask_len : ZW_NODE_MASK_BYTES;
        memset(e->multilevel_mask, 0, sizeof e->multilevel_mask);
        if (m > 0u) {
            memcpy(e->multilevel_mask, r.mask, m);
        }
        e->multilevel_mask_len = (uint8_t)m;
        e->has_multilevel_mask = true;
        e->used = true;
        s->phase = 1u;
        return IV_NEXT;
    }
    next_endpoint(s);
    return IV_NEXT;
}

static void plan_battery(const zw_iv_t *iv, const zw_iv_node_t *s, const zw_node_record_t *rec,
                         iv_ask_t *a)
{
    if (s->phase != 0u || !has(rec, ZW_CC_BATTERY)) {
        step_done(a);
        return;
    }
    ask(iv, s, a, zw_cc_battery_get_build(a->cmd, sizeof a->cmd), 0u, true);
}

static void plan_quirks(const zw_iv_t *iv, const zw_iv_node_t *s, const zw_node_record_t *rec,
                        iv_ask_t *a)
{
    zw_iv_quirk_t q;
    if (!has(rec, ZW_CC_CONFIGURATION) || !quirk(iv, rec, &q) || s->item >= q.config_count ||
        s->item >= ZW_IV_QUIRK_CONFIG_MAX) {
        step_done(a);
        return;
    }
    const zw_cc_configuration_set_t set = {q.config[s->item].parameter, false,
                                           q.config[s->item].size, q.config[s->item].value};
    ask_set(iv, s, rec, a, zw_cc_configuration_set_build(a->cmd, sizeof a->cmd, &set), 0u);
}

/* ---- Dispatch -------------------------------------------------------------------------------- */

void zw_iv_plan(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec, iv_ask_t *a)
{
    memset(a, 0, sizeof *a);
    if (s->step > ZW_IV_STEP_PROTOCOL && zw_iv_is_controller(rec)) {
        a->kind = IV_NOT_USABLE; /* network.md §10: another controller feeds no zone */
        return;
    }
    switch (s->step) {
    case ZW_IV_STEP_PROTOCOL:
        a->kind = s->phase == 0u ? IV_ASK_PROTOCOL : IV_STEP_DONE;
        break;
    case ZW_IV_STEP_PING:
        plan_ping(iv, s, rec, a);
        break;
    case ZW_IV_STEP_NODE_INFO:
        a->kind = s->phase != 0u || rec->cc_count > 0u ? IV_STEP_DONE : IV_ASK_NODE_INFO;
        break;
    case ZW_IV_STEP_SECURE_CCS:
        plan_secure(iv, s, rec, a);
        break;
    case ZW_IV_STEP_IDENTITY:
        plan_identity(iv, s, rec, a);
        break;
    case ZW_IV_STEP_VERSIONS:
        plan_versions(iv, s, rec, a);
        break;
    case ZW_IV_STEP_WAKE_UP:
        plan_wake_up(iv, s, rec, a);
        break;
    case ZW_IV_STEP_ENDPOINTS:
        plan_endpoints(iv, s, rec, a);
        break;
    case ZW_IV_STEP_ZWAVE_PLUS:
        plan_zwave_plus(iv, s, rec, a);
        break;
    case ZW_IV_STEP_LIFELINE:
        plan_lifeline(iv, s, rec, a);
        break;
    case ZW_IV_STEP_NOTIFICATION:
        plan_notification(iv, s, rec, a);
        break;
    case ZW_IV_STEP_BINARY:
        plan_binary(iv, s, rec, a);
        break;
    case ZW_IV_STEP_MULTILEVEL:
        plan_multilevel(iv, s, rec, a);
        break;
    case ZW_IV_STEP_BATTERY:
        plan_battery(iv, s, rec, a);
        break;
    case ZW_IV_STEP_QUIRKS:
        plan_quirks(iv, s, rec, a);
        break;
    default:
        step_done(a);
        break;
    }
}

iv_verdict_t zw_iv_on_answer(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec,
                             const zw_s2_rx_t *rx)
{
    static const zw_s2_rx_t NONE; /* a Set that went: no command */
    const zw_s2_rx_t *r = rx != NULL ? rx : &NONE;
    switch (s->step) {
    case ZW_IV_STEP_PING:
        s->phase = 1u;
        return IV_NEXT;
    case ZW_IV_STEP_SECURE_CCS:
        return answer_secure(iv, s, rec, r);
    case ZW_IV_STEP_IDENTITY:
        return answer_identity(iv, s, rec, r);
    case ZW_IV_STEP_VERSIONS:
        return answer_versions(iv, s, rec, r);
    case ZW_IV_STEP_WAKE_UP:
        return answer_wake_up(s, rec, r);
    case ZW_IV_STEP_ENDPOINTS:
        return answer_endpoints(iv, s, rec, r);
    case ZW_IV_STEP_ZWAVE_PLUS:
        return answer_zwave_plus(s, rec, r);
    case ZW_IV_STEP_LIFELINE:
        return answer_lifeline(iv, s, rec, r);
    case ZW_IV_STEP_NOTIFICATION:
        return answer_notification(s, rec, r);
    case ZW_IV_STEP_BINARY:
        return answer_binary(s, rec, r);
    case ZW_IV_STEP_MULTILEVEL:
        return answer_multilevel(s, rec, r);
    case ZW_IV_STEP_BATTERY:
        s->phase = 1u; /* its level goes to the owner as a report */
        return IV_NEXT;
    case ZW_IV_STEP_QUIRKS:
        s->item = (uint8_t)((unsigned)s->item + 1u);
        return IV_NEXT;
    default:
        return IV_NEXT;
    }
}

uint32_t zw_iv_on_silence(zw_iv_t *iv, zw_iv_node_t *s, zw_node_record_t *rec)
{
    if (s->step == ZW_IV_STEP_SECURE_CCS) {
        if (probe_phase(s->phase)) {
            /* not this class (Z-Wave JS: setSecurityClass(secClass, false)): the next one up */
            (void)zw_s2_set_class_result(iv->s2, s->node, PROBE[s->phase - 1u], false);
            s->phase++;
            if (!probe_phase(s->phase)) {
                probe_done(iv, s, rec);
            }
            return 0u;
        }
        /* its one class asked - a learnt one (PHASE_SINGLE) or its known highest: up to 3 queries,
         * 500 ms apart; then its classes are kept ("Let's hope for the best") */
        s->item = (uint8_t)((unsigned)s->item + 1u);
        if (s->item < SINGLE_ATTEMPTS) {
            return SINGLE_WAIT_MS;
        }
        if (s->phase == PHASE_SINGLE) {
            single_done(iv, s, rec);
        } else {
            s->phase = PHASE_DONE;
        }
        return 0u;
    }
    if (s->step == ZW_IV_STEP_ENDPOINTS && s->phase == 2u) {
        /* Z-Wave JS: "Assuming the endpoint supports all its mandatory CCs securely" - the panel
         * sends every command to an S2 node's end point at its highest class already: the next
         * end point */
        s->phase = 1u;
        s->item = (uint8_t)((unsigned)s->item + 1u);
        return 0u;
    }
    if (s->step == ZW_IV_STEP_BINARY && s->phase == PHASE_ALARM_SENSOR) {
        /* no Supported Report: no Alarm Sensor types known (BL-085). Heard and not answered:
         * asked, not again (field 12 empty). No try reached it (powered off, or no route while
         * everything went out at start): nothing written - field 12 stays missing, the interview
         * still finishes, and the next start asks again (zw_iv_alarm_types_missing, BL-130). */
        if (s->delivered) {
            no_alarm_types(&rec->endpoints[s->ep]);
            s->alarm_stored = true;
        }
        next_endpoint(s);
        return 0u;
    }
    if (s->step == ZW_IV_STEP_NOTIFICATION) {
        set_mode(&rec->endpoints[s->ep], ZW_NODE_MODE_PUSH); /* §5: nothing answers - push */
        s->phase = 3u;
        s->item = 0u;
    }
    return 0u;
}
