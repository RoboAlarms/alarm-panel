/*
 * zbz_fake_radio.c - the pretend Zigbee radio and its virtual devices (zbz_fake_radio.h;
 * docs/features/20 testing.md §2). Host builds only.
 *
 * Three parts, each the other's peer over bytes:
 *   the hooks      what zsrc_zb_radio_t asks: each encodes its frame and puts it on the air;
 *   the devices    decode what reaches them and answer with frames of their own;
 *   the panel side decodes the answers as radio_port.c does and reports them.
 * ZDO frames follow the Zigbee specification's layouts (2.4.3.1 requests, 2.4.4.2 responses; the
 * node descriptor 2.3.2.3, the power descriptor 2.3.2.4, the simple descriptor 2.3.2.5).
 */
#include "zbz_fake_radio.h"

#include <string.h>

#include "zb_signatures.h"

#define NEVER UINT64_MAX
#define HA_PROFILE 0x0104u

/* ---- bytes ------------------------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    for (size_t i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (size_t i = 0; i < 4u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

static uint64_t add_ms(uint64_t now, uint64_t ms)
{
    return (now > NEVER - ms) ? NEVER : now + ms;
}

/* ---- the log and the air ----------------------------------------------------------------------- */

static void log_frame(zbf_radio_t *r, const zbf_frame_t *f)
{
    size_t slot;
    if (r->log_len < ZBF_LOG_MAX) {
        slot = (r->log_head + r->log_len) % ZBF_LOG_MAX;
        r->log_len++;
    } else {
        slot = r->log_head;
        r->log_head = (r->log_head + 1u) % ZBF_LOG_MAX;
    }
    r->log[slot] = *f;
    r->log_total++;
}

static zbf_air_t *air_put(zbf_radio_t *r, bool to_dev, uint64_t at, uint8_t step,
                          const zbf_frame_t *f)
{
    for (size_t i = 0; i < ZBF_QUEUE_MAX; i++) {
        zbf_air_t *a = &r->air[i];
        if (!a->used) {
            memset(a, 0, sizeof(*a));
            a->used = true;
            a->to_dev = to_dev;
            a->at = at;
            a->sent = r->now;
            a->seq = r->seq++;
            a->step = step;
            a->f = *f;
            return a;
        }
    }
    r->lost++;
    return NULL;
}

static void frame_init(zbf_frame_t *f, const zbf_radio_t *r, bool rx, uint8_t kind,
                       uint64_t ieee, uint16_t nwk, uint8_t endpoint, uint16_t cluster,
                       uint32_t tag)
{
    memset(f, 0, sizeof(*f));
    f->at = r->now;
    f->rx = rx;
    f->kind = kind;
    f->ieee = ieee;
    f->nwk = nwk;
    f->endpoint = endpoint;
    f->cluster = cluster;
    f->tag = tag;
}

static bool frame_bytes(zbf_frame_t *f, const uint8_t *b, size_t len)
{
    if (len > ZBF_FRAME_MAX) {
        return false;
    }
    if (len > 0u) {
        memcpy(f->bytes, b, len);
    }
    f->len = (uint8_t)len;
    return true;
}

/* A frame's sequence number: ZCL after the frame control (and manufacturer code), ZDO first. */
static size_t tsn_pos(const zbf_frame_t *f)
{
    if (f->kind == ZBF_FRAME_ZDO) {
        return 0u;
    }
    return (f->len > 0u && ((unsigned)f->bytes[0] & ZBZ_ZCL_FC_MANUFACTURER) != 0u) ? 3u : 1u;
}

static uint8_t frame_tsn(const zbf_frame_t *f)
{
    size_t p = tsn_pos(f);
    return (p < f->len) ? f->bytes[p] : 0u;
}

/* ---- reports and the source -------------------------------------------------------------------- */

static void emit(zbf_radio_t *r, const zsrc_zb_report_t *rep)
{
    if (r->sink.report != NULL) {
        r->sink.report(r->sink.ctx, rep, r->now);
    } else if (r->src != NULL) {
        (void)zsrc_zb_on_report(r->src, rep, r->now);
    }
}

static void rep_init(zsrc_zb_report_t *rep, uint8_t kind, uint64_t ieee, uint16_t nwk,
                     uint8_t endpoint, uint32_t tag)
{
    memset(rep, 0, sizeof(*rep));
    rep->kind = kind;
    rep->ieee = ieee;
    rep->nwk = nwk;
    rep->endpoint = endpoint;
    rep->tag = tag;
}

static zbf_dev_t *dev_of(zbf_radio_t *r, uint64_t ieee)
{
    if (ieee == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < ZBF_DEV_MAX; i++) {
        if (r->dev[i].used && r->dev[i].ieee == ieee) {
            return &r->dev[i];
        }
    }
    return NULL;
}

/* ---- the panel's requests ---------------------------------------------------------------------- */

static uint8_t zcl_result(uint8_t status)
{
    switch (status) {
    case ZBZ_ZCL_SUCCESS: return ZSRC_ZB_DONE_OK;
    case ZBZ_ZCL_UNSUP_COMMAND:
    case ZBZ_ZCL_UNSUP_ATTRIBUTE:
    case ZBF_ZCL_UNREPORTABLE:
    case ZBF_ZCL_UNSUP_CLUSTER: return ZSRC_ZB_DONE_UNSUPPORTED;
    default: return ZSRC_ZB_DONE_REFUSED;
    }
}

static uint8_t zdo_result(uint8_t status)
{
    switch (status) {
    case ZBF_ZDP_SUCCESS: return ZSRC_ZB_DONE_OK;
    case ZBF_ZDP_TIMEOUT: return ZSRC_ZB_DONE_TIMEOUT;
    case ZBF_ZDP_NOT_SUPPORTED:
    case ZBF_ZDP_NOT_ACTIVE:
    case ZBF_ZDP_INVALID_EP:
    case ZBF_ZDP_NO_DESCRIPTOR: return ZSRC_ZB_DONE_UNSUPPORTED;
    default: return ZSRC_ZB_DONE_REFUSED;
    }
}

/* The request ends: its slot is freed first (a report may make the source ask again), then
 * REP_DONE when it carried a tag. */
static void rq_end(zbf_radio_t *r, zbf_rq_t *q, uint8_t result, uint8_t status)
{
    const zbf_rq_t done = *q;
    q->used = false;
    if (done.tag == 0u) {
        return;
    }
    const zbf_dev_t *d = dev_of(r, done.ieee);
    zsrc_zb_report_t rep;
    rep_init(&rep, ZSRC_ZB_REP_DONE, done.ieee, d != NULL ? d->nwk : 0u, 0u, done.tag);
    rep.u.done.step = done.step;
    rep.u.done.result = result;
    rep.u.done.zcl_status = (result == ZSRC_ZB_DONE_REFUSED) ? status : 0u;
    emit(r, &rep);
}

/* The request an answer belongs to: the same device, kind, cluster and sequence number, and for
 * ZCL the answer it waits for (or a Default Response). NULL: an answer nobody waits for. */
static zbf_rq_t *rq_match(zbf_radio_t *r, uint64_t ieee, uint8_t kind, uint16_t cluster,
                          uint8_t tsn, uint8_t answer)
{
    for (size_t i = 0; i < ZBF_RQ_MAX; i++) {
        zbf_rq_t *q = &r->rq[i];
        if (!q->used || q->ieee != ieee || q->kind != kind || q->cluster != cluster ||
            q->tsn != tsn || q->on_delivery) {
            continue;
        }
        if (kind == ZBF_FRAME_ZCL && answer != q->want &&
            !(answer == ZBZ_ZCL_DEFAULT_RSP && q->any_answer)) {
            continue;
        }
        return q;
    }
    return NULL;
}

typedef struct {
    uint64_t ieee;
    uint8_t endpoint;
    uint8_t kind;
    uint16_t cluster;
    uint8_t want;          /* ZCL: the answer's command */
    bool any_answer;
    bool on_delivery;
    uint32_t tag;
    uint8_t step;
} req_t;

/* A request on the air: logged, waited for, and on its way to the device. false: the radio is
 * out, or the air or the request table is full. */
static bool send_req(zbf_radio_t *r, const req_t *q, const uint8_t *bytes, size_t len)
{
    if (r->unplugged || len == 0u || len > ZBF_FRAME_MAX) {
        return false;
    }
    zbf_rq_t *slot = NULL;
    for (size_t i = 0; i < ZBF_RQ_MAX && slot == NULL; i++) {
        if (!r->rq[i].used) {
            slot = &r->rq[i];
        }
    }
    if (slot == NULL) {
        return false;
    }
    const zbf_dev_t *d = dev_of(r, q->ieee);
    zbf_frame_t f;
    frame_init(&f, r, false, q->kind, q->ieee, d != NULL ? d->nwk : 0u, q->endpoint, q->cluster,
               q->tag);
    (void)frame_bytes(&f, bytes, len);
    if (air_put(r, true, add_ms(r->now, ZBF_LATENCY_MS), q->step, &f) == NULL) {
        return false;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->ieee = q->ieee;
    slot->kind = q->kind;
    slot->cluster = q->cluster;
    slot->tsn = frame_tsn(&f);
    slot->want = q->want;
    slot->any_answer = q->any_answer;
    slot->on_delivery = q->on_delivery;
    slot->tag = q->tag;
    slot->step = q->step;
    slot->deadline = add_ms(r->now, ZBF_TIMEOUT_MS);
    log_frame(r, &f);
    return true;
}

/* A ZCL frame of the panel's: its sequence number is the radio's next when it asks for a new one. */
static bool send_zcl(zbf_radio_t *r, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *f,
                     bool global, uint8_t want, uint32_t tag, uint8_t step)
{
    uint8_t b[ZBZ_ZCL_FRAME_MAX];
    const uint8_t tsn = r->tsn;
    size_t n = global ? zbz_zcl_encode_global(f, tsn, b, sizeof(b))
                      : zbz_zcl_encode(f, tsn, b, sizeof(b));
    if (n == 0u) {
        return false;
    }
    const req_t q = {
        .ieee = ieee,
        .endpoint = endpoint,
        .kind = ZBF_FRAME_ZCL,
        .cluster = f->cluster,
        .want = want,
        .any_answer = true,
        .on_delivery = !global && !f->want_response,
        .tag = tag,
        .step = step,
    };
    if (!send_req(r, &q, b, n)) {
        return false;
    }
    if (f->tsn == ZSRC_ZB_TSN_NEW) {
        r->tsn = (uint8_t)(tsn + 1u);
    }
    return true;
}

static bool send_zdo(zbf_radio_t *r, uint64_t ieee, uint16_t cluster, uint8_t *b, size_t len,
                     uint32_t tag, uint8_t step)
{
    b[0] = r->zdo_tsn;
    const req_t q = {
        .ieee = ieee,
        .kind = ZBF_FRAME_ZDO,
        .cluster = cluster,
        .tag = tag,
        .step = step,
    };
    if (!send_req(r, &q, b, len)) {
        return false;
    }
    r->zdo_tsn = (uint8_t)(r->zdo_tsn + 1u);
    return true;
}

/* One of the panel's own radio's signals, reported after ZBF_LATENCY_MS. */
static bool local_signal(zbf_radio_t *r, uint16_t what, const uint8_t *b, size_t len,
                         uint32_t tag)
{
    if (r->unplugged) {
        return false;
    }
    zbf_frame_t f;
    frame_init(&f, r, true, ZBF_FRAME_NWK, 0u, 0u, 0u, what, tag);
    (void)frame_bytes(&f, b, len);
    if (air_put(r, false, add_ms(r->now, ZBF_LATENCY_MS), ZSRC_ZB_STEP_NONE, &f) == NULL) {
        return false;
    }
    log_frame(r, &f);
    return true;
}

/* ---- the devices' frames ----------------------------------------------------------------------- */

bool zbf_awake(const zbf_radio_t *r, const zbf_dev_t *d)
{
    return d != NULL && (!d->sleepy || r->now < d->awake_until);
}

/* It spoke: it listens for its wake window, and its parent hands it what it held. */
static void dev_spoke(zbf_radio_t *r, zbf_dev_t *d)
{
    uint64_t until = add_ms(r->now, d->wake_ms);
    if (until > d->awake_until) {
        d->awake_until = until;
    }
    for (size_t i = 0; i < ZBF_QUEUE_MAX; i++) {
        zbf_air_t *a = &r->air[i];
        if (a->used && a->to_dev && a->held && a->f.ieee == d->ieee) {
            a->held = false;
            a->at = add_ms(r->now, ZBF_LATENCY_MS);
        }
    }
}

/* A frame from the device to the panel, `extra_ms` later than the air takes. */
static bool dev_put(zbf_radio_t *r, zbf_dev_t *d, zbf_frame_t *f, uint32_t extra_ms)
{
    d->last = *f;
    d->has_last = true;
    dev_spoke(r, d);
    log_frame(r, f);
    if (r->unplugged) {
        return false; /* no one hears it */
    }
    return air_put(r, false, add_ms(r->now, (uint64_t)ZBF_LATENCY_MS + extra_ms), ZSRC_ZB_STEP_NONE,
                   f) != NULL;
}

static bool dev_send_zcl(zbf_radio_t *r, zbf_dev_t *d, uint8_t endpoint, const zsrc_zb_zcl_t *z,
                         bool global, uint32_t tag, const zbf_fault_t *fault)
{
    uint8_t b[ZBZ_ZCL_FRAME_MAX];
    const uint8_t tsn = d->tsn;
    size_t n = global ? zbz_zcl_encode_global(z, tsn, b, sizeof(b))
                      : zbz_zcl_encode(z, tsn, b, sizeof(b));
    if (n == 0u) {
        return false;
    }
    if (z->tsn == ZSRC_ZB_TSN_NEW) {
        d->tsn = (uint8_t)(tsn + 1u);
    }
    zbf_frame_t f;
    frame_init(&f, r, true, ZBF_FRAME_ZCL, d->ieee, d->nwk, endpoint, z->cluster, tag);
    (void)frame_bytes(&f, b, n);
    uint32_t extra = 0u;
    if (fault != NULL) {
        extra = fault->delay_ms;
        if (fault->other_tsn) {
            size_t p = tsn_pos(&f);
            f.bytes[p] = (uint8_t)(f.bytes[p] + 0x40u);
        }
    }
    bool ok = dev_put(r, d, &f, extra);
    if (ok && fault != NULL && fault->duplicate) {
        ok = dev_put(r, d, &f, extra);
    }
    return ok;
}

static bool dev_send_zdo(zbf_radio_t *r, zbf_dev_t *d, uint16_t cluster, const uint8_t *b,
                         size_t len, uint32_t tag, const zbf_fault_t *fault)
{
    zbf_frame_t f;
    frame_init(&f, r, true, ZBF_FRAME_ZDO, d->ieee, d->nwk, 0u, cluster, tag);
    if (!frame_bytes(&f, b, len)) {
        return false;
    }
    uint32_t extra = 0u;
    if (fault != NULL) {
        extra = fault->delay_ms;
        if (fault->other_tsn && f.len > 0u) {
            f.bytes[0] = (uint8_t)(f.bytes[0] + 0x40u);
        }
    }
    bool ok = dev_put(r, d, &f, extra);
    if (ok && fault != NULL && fault->duplicate) {
        ok = dev_put(r, d, &f, extra);
    }
    return ok;
}

static bool dev_signal(zbf_radio_t *r, zbf_dev_t *d, uint16_t what, const uint8_t *b, size_t len)
{
    zbf_frame_t f;
    frame_init(&f, r, true, ZBF_FRAME_NWK, d->ieee, d->nwk, 0u, what, 0u);
    (void)frame_bytes(&f, b, len);
    log_frame(r, &f);
    if (r->unplugged) {
        return false;
    }
    return air_put(r, false, add_ms(r->now, ZBF_LATENCY_MS), ZSRC_ZB_STEP_NONE, &f) != NULL;
}

static bool dev_announce(zbf_radio_t *r, zbf_dev_t *d)
{
    uint8_t b[12];
    b[0] = d->tsn;
    d->tsn = (uint8_t)(d->tsn + 1u);
    wr16(&b[1], d->nwk);
    wr64(&b[3], d->ieee);
    b[11] = d->node.mac_caps;
    return dev_send_zdo(r, d, ZBF_ZDO_ANNOUNCE, b, sizeof(b), 0u, NULL);
}

/* ---- a device's endpoints and attributes ------------------------------------------------------- */

zbf_ep_t *zbf_ep(zbf_dev_t *d, uint8_t endpoint)
{
    if (d == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < d->ep_count && i < ZSRC_ZB_EP_MAX; i++) {
        if (d->ep[i].sd.endpoint == endpoint) {
            return &d->ep[i];
        }
    }
    return NULL;
}

static bool has_in(const zbf_ep_t *e, uint16_t cluster)
{
    for (uint8_t i = 0; i < e->sd.in_count && i < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
        if (e->sd.in[i] == cluster) {
            return true;
        }
    }
    return false;
}

static bool has_out(const zbf_ep_t *e, uint16_t cluster)
{
    for (uint8_t i = 0; i < e->sd.out_count && i < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
        if (e->sd.out[i] == cluster) {
            return true;
        }
    }
    return false;
}

static zbf_ep_t *ep_with(zbf_dev_t *d, uint16_t cluster)
{
    for (uint8_t i = 0; i < d->ep_count && i < ZSRC_ZB_EP_MAX; i++) {
        if (d->ep[i].sd.profile == HA_PROFILE && has_in(&d->ep[i], cluster)) {
            return &d->ep[i];
        }
    }
    return NULL;
}

static uint16_t ias_word(const zbf_radio_t *r, const zbf_ep_t *e)
{
    uint16_t s = e->status;
    if (e->test_until != 0u && r->now < e->test_until) {
        s = (uint16_t)(s | ZSRC_ZB_IAS_TEST);
    }
    return s;
}

/* An attribute's value as the device holds it: false when it has no such attribute. `buf` holds
 * the value (strings point at the device's own text). */
typedef struct {
    uint8_t type;
    uint8_t len;
    const uint8_t *value;
    uint8_t buf[8];
} attr_val_t;

static void val_num(attr_val_t *v, uint8_t type, uint64_t n, uint8_t len)
{
    v->type = type;
    v->len = len;
    for (uint8_t i = 0; i < len && i < 8u; i++) {
        v->buf[i] = (uint8_t)((n >> (8u * i)) & 0xFFu);
    }
    v->value = v->buf;
}

static void val_text(attr_val_t *v, const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    v->type = ZBZ_ZCL_TYPE_STRING;
    v->len = (uint8_t)n;
    v->value = (const uint8_t *)s;
}

static bool dev_attr(const zbf_radio_t *r, const zbf_dev_t *d, const zbf_ep_t *e, uint16_t cluster,
                     uint16_t attr, attr_val_t *v)
{
    static const char date_code[] = "20260101";
    static const char sw_build[] = "1.0.0";
    memset(v, 0, sizeof(*v));
    switch (cluster) {
    case ZSRC_ZB_ZCL_BASIC:
        switch (attr) {
        case 0x0000u: val_num(v, ZBZ_ZCL_TYPE_U8, 3u, 1u); return true; /* ZCL version */
        case 0x0001u: val_num(v, ZBZ_ZCL_TYPE_U8, 1u, 1u); return true; /* application */
        case 0x0002u: val_num(v, ZBZ_ZCL_TYPE_U8, 2u, 1u); return true; /* stack */
        case 0x0003u: val_num(v, ZBZ_ZCL_TYPE_U8, 1u, 1u); return true; /* hardware */
        case 0x0004u: val_text(v, d->manufacturer, sizeof(d->manufacturer)); return true;
        case 0x0005u: val_text(v, d->model, sizeof(d->model)); return true;
        case 0x0006u: val_text(v, date_code, sizeof(date_code)); return true;
        case 0x0007u: val_num(v, ZBZ_ZCL_TYPE_ENUM8, d->power_source, 1u); return true;
        case 0x4000u: val_text(v, sw_build, sizeof(sw_build)); return true;
        default: return false;
        }
    case ZSRC_ZB_ZCL_POWER_CFG:
        if (attr == 0x0020u) {
            val_num(v, ZBZ_ZCL_TYPE_U8, 30u, 1u); /* 3.0 V */
            return true;
        }
        if (attr == 0x0021u) {
            uint8_t half = (d->battery_pct == ZSRC_ZB_BATTERY_UNKNOWN)
                               ? 0xFFu
                               : (uint8_t)((d->battery_pct > 100u ? 100u : d->battery_pct) * 2u);
            val_num(v, ZBZ_ZCL_TYPE_U8, half, 1u);
            return true;
        }
        return false;
    case ZSRC_ZB_ZCL_IAS_ZONE:
        switch (attr) {
        case ZBZ_IAS_ATTR_ZONE_STATE: val_num(v, ZBZ_ZCL_TYPE_ENUM8, e->enrolled ? 1u : 0u, 1u); return true;
        case ZBZ_IAS_ATTR_ZONE_TYPE: val_num(v, ZBZ_ZCL_TYPE_ENUM16, e->zone_type, 2u); return true;
        case ZBZ_IAS_ATTR_ZONE_STATUS: val_num(v, ZBZ_ZCL_TYPE_MAP16, ias_word(r, e), 2u); return true;
        case ZBZ_IAS_ATTR_CIE_ADDRESS: val_num(v, ZBZ_ZCL_TYPE_EUI64, e->cie, 8u); return true;
        case ZBZ_IAS_ATTR_ZONE_ID: val_num(v, ZBZ_ZCL_TYPE_U8, e->zone_id, 1u); return true;
        default: return false;
        }
    case ZSRC_ZB_ZCL_POLL_CTRL:
        if (attr == 0x0000u) { /* Check-in Interval, quarter seconds (default 1 h) */
            uint32_t s = d->check_in_s != 0u ? d->check_in_s : 3600u;
            val_num(v, ZBZ_ZCL_TYPE_U32, (uint64_t)s * 4u, 4u);
            return true;
        }
        return false;
    case ZSRC_ZB_ZCL_ON_OFF:
        if (attr == 0x0000u) {
            val_num(v, ZBZ_ZCL_TYPE_BOOL, e->on ? 1u : 0u, 1u);
            return true;
        }
        return false;
    case ZSRC_ZB_ZCL_OCCUPANCY:
        if (attr == 0x0000u) {
            val_num(v, ZBZ_ZCL_TYPE_MAP8, e->on ? 1u : 0u, 1u);
            return true;
        }
        return false;
    case ZSRC_ZB_ZCL_TEMP_MEAS:
        if (attr == 0x0000u) {
            val_num(v, ZBZ_ZCL_TYPE_S16, (uint16_t)e->temp_x100, 2u);
            return true;
        }
        return false;
    case ZSRC_ZB_ZCL_IAS_WD:
        if (attr == 0x0000u) { /* MaxDuration: the default, or its own (ZB03) */
            const uint8_t type = d->wd_max_type != 0u ? d->wd_max_type : ZBZ_ZCL_TYPE_U16;
            const uint8_t len = type == ZBZ_ZCL_TYPE_U8 ? 1u : 2u;
            val_num(v, type, d->wd_max_set ? d->wd_max_s : 240u, len);
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* ---- a device receives ------------------------------------------------------------------------- */

static bool fault_set(const zbf_fault_t *f)
{
    return f->drop != 0u || f->delay_ms != 0u || f->duplicate || f->other_tsn || f->status != 0u ||
           f->timeout;
}

static zbf_fault_t *fault_for(zbf_dev_t *d, uint8_t step)
{
    if (step <= ZSRC_ZB_STEP_DONE && fault_set(&d->fault[step])) {
        return &d->fault[step];
    }
    return &d->fault[ZSRC_ZB_STEP_NONE];
}

/* A Default Response from the device to the panel's command (direction reversed). */
static void dev_default_rsp(zbf_radio_t *r, zbf_dev_t *d, const zbf_air_t *a,
                            const zsrc_zb_zcl_t *req, uint8_t status, const zbf_fault_t *fault)
{
    zsrc_zb_zcl_t rsp;
    zbz_zcl_default_rsp(req->cluster, req->command, status, !req->server_to_client, &rsp);
    rsp.tsn = req->tsn;
    (void)dev_send_zcl(r, d, a->f.endpoint, &rsp, true, a->f.tag, fault);
}

static void dev_read(zbf_radio_t *r, zbf_dev_t *d, zbf_ep_t *e, const zbf_air_t *a,
                     const zsrc_zb_zcl_t *req, const zbf_fault_t *fault)
{
    zsrc_zb_zcl_t rsp;
    zbz_zcl_global(req->cluster, ZBZ_ZCL_READ_ATTR_RSP, !req->server_to_client, &rsp);
    rsp.want_response = false;
    rsp.tsn = req->tsn;
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t rec;
    zbz_zcl_iter(&it, req);
    while (zbz_zcl_next_attr(&it, &rec)) {
        attr_val_t v;
        zbz_zcl_attr_t out = {.attr = rec.attr};
        if (fault->status != 0u) {
            out.status = fault->status;
        } else if (dev_attr(r, d, e, req->cluster, rec.attr, &v)) {
            out.status = ZBZ_ZCL_SUCCESS;
            out.type = v.type;
            out.len = v.len;
            out.value = (v.len > 0u) ? v.value : NULL;
        } else {
            out.status = ZBZ_ZCL_UNSUP_ATTRIBUTE;
        }
        if (!zbz_zcl_put_attr(&rsp, &out)) {
            break; /* as many as fit: the panel asks again for the rest */
        }
    }
    (void)dev_send_zcl(r, d, a->f.endpoint, &rsp, true, a->f.tag, fault);
}

static void dev_enroll_request(zbf_radio_t *r, zbf_dev_t *d, const zbf_ep_t *e)
{
    zsrc_zb_zcl_t z;
    memset(&z, 0, sizeof(z));
    z.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    z.command = ZBZ_IAS_ENROLL_REQUEST;
    z.server_to_client = true;
    z.want_response = false;
    z.tsn = ZSRC_ZB_TSN_NEW;
    wr16(&z.payload[0], e->zone_type);
    wr16(&z.payload[2], d->node.manufacturer_code);
    z.len = 4;
    (void)dev_send_zcl(r, d, e->sd.endpoint, &z, false, 0u, NULL);
}

static void dev_write(zbf_radio_t *r, zbf_dev_t *d, zbf_ep_t *e, const zbf_air_t *a,
                      const zsrc_zb_zcl_t *req, const zbf_fault_t *fault)
{
    zsrc_zb_zcl_t rsp;
    zbz_zcl_global(req->cluster, ZBZ_ZCL_WRITE_ATTR_RSP, !req->server_to_client, &rsp);
    rsp.want_response = false;
    rsp.tsn = req->tsn;
    bool all_ok = true;
    bool cie_written = false;
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t rec;
    zbz_zcl_iter(&it, req);
    while (zbz_zcl_next_attr(&it, &rec)) {
        uint8_t status = fault->status;
        if (status == 0u) {
            if (req->cluster == ZSRC_ZB_ZCL_IAS_ZONE && rec.attr == ZBZ_IAS_ATTR_CIE_ADDRESS &&
                rec.type == ZBZ_ZCL_TYPE_EUI64) {
                e->cie = zbz_zcl_attr_uint(&rec);
                cie_written = true;
            } else {
                status = ZBZ_ZCL_UNSUP_ATTRIBUTE;
            }
        }
        if (status != ZBZ_ZCL_SUCCESS) {
            all_ok = false;
            const zbz_zcl_attr_t out = {.attr = rec.attr, .status = status};
            (void)zbz_zcl_put_attr(&rsp, &out);
        }
    }
    if (all_ok) {
        const zbz_zcl_attr_t lone = {.attr = ZBZ_ZCL_ATTR_ALL, .status = ZBZ_ZCL_SUCCESS};
        (void)zbz_zcl_put_attr(&rsp, &lone);
    }
    if (req->command != ZBZ_ZCL_WRITE_ATTR_NORSP) {
        (void)dev_send_zcl(r, d, a->f.endpoint, &rsp, true, a->f.tag, fault);
    }
    /* An IAS device asks to enroll once it knows its CIE (ZCL 8.2.2.2.3, "auto enroll request"). */
    if (cie_written && d->auto_enroll && e->zone_type != ZSRC_ZB_ZONE_TYPE_NONE && !e->enrolled) {
        dev_enroll_request(r, d, e);
    }
}

static bool reportable(const zbf_ep_t *e, uint16_t cluster, uint16_t attr)
{
    switch (cluster) {
    case ZSRC_ZB_ZCL_POWER_CFG: return attr == 0x0020u || attr == 0x0021u;
    case ZSRC_ZB_ZCL_ON_OFF:
    case ZSRC_ZB_ZCL_OCCUPANCY:
    case ZSRC_ZB_ZCL_TEMP_MEAS: return attr == 0x0000u;
    case ZSRC_ZB_ZCL_IAS_ZONE: return attr == ZBZ_IAS_ATTR_ZONE_STATUS;
    default: (void)e; return false;
    }
}

static void dev_configure(zbf_radio_t *r, zbf_dev_t *d, zbf_ep_t *e, const zbf_air_t *a,
                          const zsrc_zb_zcl_t *req, const zbf_fault_t *fault)
{
    zsrc_zb_reporting_t recs[6];
    uint8_t count = 0;
    if (zbz_zcl_decode_config_report(req, recs, (uint8_t)(sizeof(recs) / sizeof(recs[0])),
                                     &count) != ZBZ_ZCL_OK) {
        dev_default_rsp(r, d, a, req, 0x80u /* MALFORMED_COMMAND */, fault);
        return;
    }
    zsrc_zb_zcl_t rsp;
    zbz_zcl_global(req->cluster, ZBZ_ZCL_CONFIG_REPORT_RSP, !req->server_to_client, &rsp);
    rsp.want_response = false;
    rsp.tsn = req->tsn;
    bool all_ok = true;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t status = fault->status;
        if (status == 0u && !reportable(e, req->cluster, recs[i].attr)) {
            status = ZBZ_ZCL_UNSUP_ATTRIBUTE;
        }
        if (status != ZBZ_ZCL_SUCCESS) {
            all_ok = false;
            const zbz_zcl_attr_t out = {.attr = recs[i].attr, .status = status};
            (void)zbz_zcl_put_attr(&rsp, &out);
        }
    }
    if (all_ok) {
        const zbz_zcl_attr_t lone = {.attr = ZBZ_ZCL_ATTR_ALL, .status = ZBZ_ZCL_SUCCESS};
        (void)zbz_zcl_put_attr(&rsp, &lone);
    }
    (void)dev_send_zcl(r, d, a->f.endpoint, &rsp, true, a->f.tag, fault);
}

/* A cluster command from the panel: what it does to the device, and its status. */
static uint8_t dev_command(zbf_radio_t *r, zbf_ep_t *e, const zsrc_zb_zcl_t *c)
{
    e->commands++;
    e->last_cmd = *c;
    if (c->manufacturer != 0u) {
        return ZBZ_ZCL_UNSUP_COMMAND;
    }
    switch (c->cluster) {
    case ZSRC_ZB_ZCL_IAS_ZONE:
        if (c->server_to_client) {
            return ZBZ_ZCL_UNSUP_COMMAND;
        }
        switch (c->command) {
        case ZBZ_IAS_ENROLL_RESPONSE:
            if (c->len < 2u) {
                return 0x80u; /* MALFORMED_COMMAND */
            }
            if (c->payload[0] == ZBZ_IAS_ENROLL_OK) {
                e->enrolled = true;
                e->zone_id = c->payload[1];
            }
            return ZBZ_ZCL_SUCCESS;
        case ZBZ_IAS_NORMAL_MODE: e->test_until = 0u; return ZBZ_ZCL_SUCCESS;
        case ZBZ_IAS_TEST_MODE:
            if (c->len < 1u) {
                return 0x80u;
            }
            e->test_until = add_ms(r->now, (uint64_t)c->payload[0] * 1000u);
            return ZBZ_ZCL_SUCCESS;
        default: return ZBZ_ZCL_UNSUP_COMMAND;
        }
    case ZSRC_ZB_ZCL_IAS_WD:
        if (c->command == 0x00u) { /* Start Warning */
            e->warnings++;
        }
        return (c->command <= 0x01u) ? ZBZ_ZCL_SUCCESS : ZBZ_ZCL_UNSUP_COMMAND;
    case ZSRC_ZB_ZCL_IDENTIFY:
        return (c->command <= 0x01u) ? ZBZ_ZCL_SUCCESS : ZBZ_ZCL_UNSUP_COMMAND;
    default: return ZBZ_ZCL_SUCCESS;
    }
}

static void dev_zcl(zbf_radio_t *r, zbf_dev_t *d, const zbf_air_t *a, const zbf_fault_t *fault)
{
    zbf_ep_t *e = zbf_ep(d, a->f.endpoint);
    if (e == NULL) {
        return; /* no such endpoint: the APS layer drops it */
    }
    const bool global = zbz_zcl_is_global(a->f.bytes, a->f.len);
    zsrc_zb_zcl_t req;
    zbz_zcl_result_t res = global ? zbz_zcl_decode_global(a->f.bytes, a->f.len, a->f.cluster, &req)
                                  : zbz_zcl_decode(a->f.bytes, a->f.len, a->f.cluster, &req);
    if (res != ZBZ_ZCL_OK) {
        return;
    }
    /* A command to the cluster's server needs it on the input list; one from it (an ACE answer
     * to a keypad) on the output list. */
    const bool there = req.server_to_client ? has_out(e, req.cluster) : has_in(e, req.cluster);
    if (!there) {
        dev_default_rsp(r, d, a, &req, ZBF_ZCL_UNSUP_CLUSTER, NULL);
        return;
    }
    if (global) {
        switch (req.command) {
        case ZBZ_ZCL_READ_ATTR: dev_read(r, d, e, a, &req, fault); return;
        case ZBZ_ZCL_WRITE_ATTR:
        case ZBZ_ZCL_WRITE_ATTR_UNDIV:
        case ZBZ_ZCL_WRITE_ATTR_NORSP: dev_write(r, d, e, a, &req, fault); return;
        case ZBZ_ZCL_CONFIG_REPORT: dev_configure(r, d, e, a, &req, fault); return;
        case ZBZ_ZCL_DEFAULT_RSP: return;
        default: dev_default_rsp(r, d, a, &req, 0x82u /* UNSUP_GENERAL_COMMAND */, fault); return;
        }
    }
    uint8_t status = dev_command(r, e, &req);
    if (fault->status != 0u) {
        status = fault->status;
    }
    /* A Default Response when asked for, and for every failure (ZCL 2.5.12.2). */
    if (req.want_response || status != ZBZ_ZCL_SUCCESS) {
        dev_default_rsp(r, d, a, &req, status, fault);
    }
}

static void dev_zdo(zbf_radio_t *r, zbf_dev_t *d, const zbf_air_t *a, const zbf_fault_t *fault)
{
    const uint8_t *q = a->f.bytes;
    const size_t qn = a->f.len;
    uint8_t b[ZBF_FRAME_MAX];
    size_t n = 0;
    uint8_t status = ZBF_ZDP_SUCCESS;
    if (qn < 1u) {
        return;
    }
    b[n++] = q[0];
    switch (a->f.cluster) {
    case ZBF_ZDO_NODE_DESC:
    case ZBF_ZDO_POWER_DESC:
    case ZBF_ZDO_ACTIVE_EP: {
        if (qn < 3u) {
            return;
        }
        if (rd16(&q[1]) != d->nwk) {
            status = ZBF_ZDP_NOT_FOUND;
        }
        if (fault->status != 0u) {
            status = fault->status;
        }
        b[n++] = status;
        wr16(&b[n], d->nwk);
        n += 2u;
        if (status != ZBF_ZDP_SUCCESS) {
            break;
        }
        if (a->f.cluster == ZBF_ZDO_NODE_DESC) {
            b[n++] = (uint8_t)(d->node.logical_type & 0x07u);
            b[n++] = 0x40u; /* APS flags 0, 2.4 GHz band */
            b[n++] = d->node.mac_caps;
            wr16(&b[n], d->node.manufacturer_code);
            n += 2u;
            b[n++] = 0x52u; /* max buffer size */
            wr16(&b[n], d->node.max_in_transfer != 0u ? d->node.max_in_transfer : 0x0052u);
            n += 2u;
            wr16(&b[n], d->node.server_mask);
            n += 2u;
            wr16(&b[n], 0x0052u); /* max outgoing transfer */
            n += 2u;
            b[n++] = 0x00u;       /* descriptor capability */
        } else if (a->f.cluster == ZBF_ZDO_POWER_DESC) {
            const bool mains = (d->power_source & 0x7Fu) == 0x01u ||
                               (d->power_source & 0x7Fu) == 0x02u ||
                               (d->power_source & 0x7Fu) == 0x04u;
            const uint8_t source = (uint8_t)(mains ? 0x01u : 0x04u);
            uint8_t level = 0x0Cu;
            if (!mains && d->battery_pct != ZSRC_ZB_BATTERY_UNKNOWN) {
                level = (uint8_t)(d->battery_pct >= 100u  ? 0x0Cu
                                  : d->battery_pct >= 66u ? 0x08u
                                  : d->battery_pct >= 33u ? 0x04u
                                                          : 0x00u);
            }
            const uint8_t mode = (uint8_t)(d->sleepy ? 0x01u : 0x00u);
            b[n++] = (uint8_t)(mode | (uint8_t)(source << 4));
            b[n++] = (uint8_t)(source | (uint8_t)(level << 4));
        } else {
            const bool extra = d->listed_only_ep != 0u && zbf_ep(d, d->listed_only_ep) == NULL;
            b[n++] = (uint8_t)(d->ep_count + (extra ? 1u : 0u));
            for (uint8_t i = 0; i < d->ep_count; i++) {
                b[n++] = d->ep[i].sd.endpoint;
            }
            if (extra) {
                b[n++] = d->listed_only_ep; /* listed, and no descriptor behind it */
            }
        }
        break;
    }
    case ZBF_ZDO_SIMPLE_DESC: {
        if (qn < 4u) {
            return;
        }
        const zbf_ep_t *e = zbf_ep(d, q[3]);
        if (rd16(&q[1]) != d->nwk) {
            status = ZBF_ZDP_NOT_FOUND;
        } else if (q[3] == 0u || (q[3] > 240u && q[3] != 242u)) {
            status = ZBF_ZDP_INVALID_EP;
        } else if (e == NULL) {
            status = ZBF_ZDP_NOT_ACTIVE;
        }
        if (fault->status != 0u) {
            status = fault->status;
        }
        b[n++] = status;
        wr16(&b[n], d->nwk);
        n += 2u;
        if (status != ZBF_ZDP_SUCCESS || e == NULL) {
            b[n++] = 0u; /* length 0: no descriptor */
            break;
        }
        const size_t len_at = n++;
        b[n++] = e->sd.endpoint;
        wr16(&b[n], e->sd.profile);
        n += 2u;
        wr16(&b[n], e->sd.device_id);
        n += 2u;
        b[n++] = (uint8_t)(e->sd.version & 0x0Fu);
        b[n++] = e->sd.in_count;
        for (uint8_t i = 0; i < e->sd.in_count && i < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
            wr16(&b[n], e->sd.in[i]);
            n += 2u;
        }
        b[n++] = e->sd.out_count;
        for (uint8_t i = 0; i < e->sd.out_count && i < ZSRC_ZB_EP_CLUSTERS_MAX; i++) {
            wr16(&b[n], e->sd.out[i]);
            n += 2u;
        }
        b[len_at] = (uint8_t)(n - len_at - 1u);
        break;
    }
    case ZBF_ZDO_BIND: {
        if (qn < 22u) {
            return;
        }
        const zbf_ep_t *e = zbf_ep(d, q[9]);
        const uint16_t cluster = rd16(&q[10]);
        if (rd64(&q[1]) != d->ieee || e == NULL) {
            status = ZBF_ZDP_INVALID_EP;
        } else if (!has_in(e, cluster) && !has_out(e, cluster)) {
            status = ZBF_ZDP_NOT_SUPPORTED;
        }
        if (fault->status != 0u) {
            status = fault->status;
        }
        b[n++] = status;
        break;
    }
    case ZBF_ZDO_LEAVE: {
        if (qn < 10u) {
            return;
        }
        if (rd64(&q[1]) != d->ieee) {
            status = ZBF_ZDP_NOT_FOUND;
        }
        if (fault->status != 0u) {
            status = fault->status;
        }
        b[n++] = status;
        (void)dev_send_zdo(r, d, (uint16_t)(a->f.cluster | ZBF_ZDO_RSP), b, n, a->f.tag, fault);
        if (status == ZBF_ZDP_SUCCESS) {
            d->joined = false; /* it answers, then goes */
        }
        return;
    }
    default: return; /* not a request a device answers */
    }
    (void)dev_send_zdo(r, d, (uint16_t)(a->f.cluster | ZBF_ZDO_RSP), b, n, a->f.tag, fault);
}

static zbf_rq_t *rq_of_request(zbf_radio_t *r, const zbf_frame_t *f)
{
    const uint8_t tsn = frame_tsn(f);
    for (size_t i = 0; i < ZBF_RQ_MAX; i++) {
        zbf_rq_t *q = &r->rq[i];
        if (q->used && q->ieee == f->ieee && q->kind == f->kind && q->cluster == f->cluster &&
            q->tsn == tsn) {
            return q;
        }
    }
    return NULL;
}

static void dev_receive(zbf_radio_t *r, zbf_dev_t *d, const zbf_air_t *a)
{
    zbf_fault_t *fault = fault_for(d, a->step);
    if (fault->drop > 0u) {
        fault->drop--;
        return; /* lost on the way: the request times out */
    }
    if (fault->timeout) {
        return; /* it never answers, not even the APS ack */
    }
    d->requests++;
    zbf_rq_t *q = rq_of_request(r, &a->f);
    if (q != NULL && q->on_delivery) {
        rq_end(r, q, ZSRC_ZB_DONE_OK, 0u);
    }
    if (a->f.kind == ZBF_FRAME_ZDO) {
        dev_zdo(r, d, a, fault);
    } else if (a->f.kind == ZBF_FRAME_ZCL) {
        dev_zcl(r, d, a, fault);
    }
}

/* ---- the panel side: what radio_port.c does with what arrives ---------------------------------- */

static void frame_seen(zbf_radio_t *r, const zbf_dev_t *d, uint64_t ieee, uint16_t nwk,
                       uint8_t endpoint)
{
    const uint8_t lqi = d != NULL ? d->lqi : 0u;
    const int8_t rssi = d != NULL ? d->rssi : 0;
    if (r->src != NULL) {
        zsrc_zb_frame_seen(r->src, ieee, lqi, r->now);
    }
    if (lqi != 0u || rssi != 0) {
        zsrc_zb_report_t rep;
        rep_init(&rep, ZSRC_ZB_REP_LINK, ieee, nwk, endpoint, 0u);
        rep.u.link.lqi = lqi;
        rep.u.link.rssi = rssi;
        emit(r, &rep);
    }
}

static void text_attr(char *dst, size_t cap, const zbz_zcl_attr_t *a)
{
    size_t n = (a->len < cap - 1u) ? a->len : cap - 1u;
    memset(dst, 0, cap);
    if (n > 0u && a->value != NULL) {
        memcpy(dst, a->value, n);
    }
}

static void basic_attr(zsrc_zb_basic_t *b, const zbz_zcl_attr_t *a)
{
    const bool text = a->type == ZBZ_ZCL_TYPE_STRING;
    const uint8_t v = (uint8_t)zbz_zcl_attr_uint(a);
    switch (a->attr) {
    case 0x0000u: b->zcl_version = v; b->have |= ZSRC_ZB_BASIC_ZCL_VERSION; break;
    case 0x0001u: b->app_version = v; b->have |= ZSRC_ZB_BASIC_APP_VERSION; break;
    case 0x0003u: b->hw_version = v; b->have |= ZSRC_ZB_BASIC_HW_VERSION; break;
    case 0x0007u: b->power_source = v; b->have |= ZSRC_ZB_BASIC_POWER; break;
    case 0x0004u:
        if (text) {
            text_attr(b->manufacturer, sizeof(b->manufacturer), a);
            b->have |= ZSRC_ZB_BASIC_MANUFACTURER;
        }
        break;
    case 0x0005u:
        if (text) {
            text_attr(b->model, sizeof(b->model), a);
            b->have |= ZSRC_ZB_BASIC_MODEL;
        }
        break;
    case 0x0006u:
        if (text) {
            text_attr(b->date_code, sizeof(b->date_code), a);
            b->have |= ZSRC_ZB_BASIC_DATE_CODE;
        }
        break;
    case 0x4000u:
        if (text) {
            text_attr(b->sw_build, sizeof(b->sw_build), a);
            b->have |= ZSRC_ZB_BASIC_SW_BUILD;
        }
        break;
    default: break;
    }
}

/* One attribute outside Basic: the source's inbound call, then the attribute sink. */
static void attribute(zbf_radio_t *r, uint64_t ieee, uint8_t ep, uint16_t cluster,
                      const zbz_zcl_attr_t *a)
{
    const uint64_t v = zbz_zcl_attr_uint(a);
    zsrc_zb_t *z = r->src;
    if (z != NULL) {
        switch (cluster) {
        case ZSRC_ZB_ZCL_IAS_ZONE:
            if (a->attr == ZBZ_IAS_ATTR_ZONE_STATUS) {
                zsrc_zb_ias_status(z, ieee, ep, (uint16_t)v, r->now);
            } else if (a->attr == ZBZ_IAS_ATTR_ZONE_TYPE && v != ZSRC_ZB_ZONE_TYPE_NONE) {
                /* As the firmware's radio port does (radio_port.c attribute()): the zone type
                 * read back after the join describes the device to the source again. */
                const zsrc_zb_device_t *s = zsrc_zb_device(z, ieee, ep);
                if (s != NULL) {
                    zsrc_zb_desc_t d;
                    memset(&d, 0, sizeof(d));
                    d.ieee = ieee;
                    d.nwk = s->nwk;
                    d.endpoint = ep;
                    d.clusters = s->clusters;
                    d.zone_type = (uint16_t)v;
                    d.router = s->router;
                    memcpy(d.manufacturer, s->manufacturer, sizeof(d.manufacturer));
                    memcpy(d.model, s->model, sizeof(d.model));
                    d.join = s->join;
                    (void)zsrc_zb_device_seen(z, &d, r->now);
                }
            }
            break;
        case ZSRC_ZB_ZCL_ON_OFF:
            if (a->attr == 0x0000u) {
                zsrc_zb_attr_bool(z, ieee, ep, cluster, v != 0u, r->now);
            }
            break;
        case ZSRC_ZB_ZCL_OCCUPANCY:
            if (a->attr == 0x0000u) {
                zsrc_zb_attr_bool(z, ieee, ep, cluster, (v & 0x01u) != 0u, r->now);
            }
            break;
        case ZSRC_ZB_ZCL_TEMP_MEAS:
            if (a->attr == 0x0000u) {
                zsrc_zb_attr_value(z, ieee, ep, cluster, (int32_t)zbz_zcl_attr_int(a), r->now);
            }
            break;
        case ZSRC_ZB_ZCL_POWER_CFG:
            /* Battery percentage remaining, in half percents; 0xFF is "unknown", not 100 %. */
            if (a->attr == 0x0021u && v != 0xFFu) {
                zsrc_zb_battery(z, ieee, (uint8_t)(v / 2u), r->now);
            }
            break;
        case ZSRC_ZB_ZCL_POLL_CTRL:
            if (a->attr == 0x0000u) {
                zsrc_zb_check_in_interval(z, ieee, (uint32_t)(v / 4u), r->now);
            }
            break;
        default: break;
        }
    }
    if (r->sink.attr != NULL && a->type != ZBZ_ZCL_TYPE_STRING && a->type != ZBZ_ZCL_TYPE_OCTETS) {
        r->sink.attr(r->sink.ctx, ieee, ep, cluster, a->attr, v, r->now);
    }
}

/* The records of a Read Attributes Response or a Report Attributes. */
static void panel_attrs(zbf_radio_t *r, const zbf_frame_t *f, const zsrc_zb_zcl_t *z,
                        uint32_t tag, bool *any_ok, uint8_t *first_fail)
{
    zbz_zcl_iter_t it;
    zbz_zcl_attr_t a;
    zsrc_zb_report_t rep;
    const bool basic = z->cluster == ZSRC_ZB_ZCL_BASIC;
    rep_init(&rep, ZSRC_ZB_REP_BASIC, f->ieee, f->nwk, f->endpoint, tag);
    *any_ok = false;
    *first_fail = ZBZ_ZCL_SUCCESS;
    zbz_zcl_iter(&it, z);
    while (zbz_zcl_next_attr(&it, &a)) {
        if (a.status != ZBZ_ZCL_SUCCESS) {
            if (*first_fail == ZBZ_ZCL_SUCCESS) {
                *first_fail = a.status;
            }
            continue;
        }
        *any_ok = true;
        if (basic) {
            basic_attr(&rep.u.basic, &a);
        } else {
            attribute(r, f->ieee, f->endpoint, z->cluster, &a);
        }
    }
    if (basic && (rep.u.basic.have != 0u || tag != 0u)) {
        emit(r, &rep);
    }
}

static void panel_zcl(zbf_radio_t *r, const zbf_frame_t *f)
{
    const zbf_dev_t *d = dev_of(r, f->ieee);
    const bool global = zbz_zcl_is_global(f->bytes, f->len);
    zsrc_zb_zcl_t z;
    zbz_zcl_result_t res = global ? zbz_zcl_decode_global(f->bytes, f->len, f->cluster, &z)
                                  : zbz_zcl_decode(f->bytes, f->len, f->cluster, &z);
    if (res != ZBZ_ZCL_OK) {
        return; /* the stack drops a frame it can't read */
    }
    frame_seen(r, d, f->ieee, f->nwk, f->endpoint);
    const uint8_t tsn = (uint8_t)z.tsn;
    if (global) {
        zbf_rq_t *q = rq_match(r, f->ieee, ZBF_FRAME_ZCL, z.cluster, tsn, z.command);
        bool any_ok = false;
        uint8_t fail = ZBZ_ZCL_SUCCESS;
        switch (z.command) {
        case ZBZ_ZCL_READ_ATTR_RSP:
            panel_attrs(r, f, &z, q != NULL ? q->tag : 0u, &any_ok, &fail);
            if (z.cluster == ZSRC_ZB_ZCL_IAS_WD) {
                /* A siren's answer (its MaxDuration, ZB03), its records as they came, for the
                 * panel - before the request's end, as every answer (radio_port.c to follow). */
                zsrc_zb_report_t rep;
                rep_init(&rep, ZSRC_ZB_REP_ZCL, f->ieee, f->nwk, f->endpoint, 0u);
                rep.u.zcl = z;
                emit(r, &rep);
            }
            if (q != NULL) {
                /* answered when any attribute was there (radio_port.c) */
                const uint8_t ended = (any_ok || fail == ZBZ_ZCL_SUCCESS)
                                        ? (uint8_t)ZSRC_ZB_DONE_OK
                                        : zcl_result(fail);
                rq_end(r, q, ended, fail);
            }
            return;
        case ZBZ_ZCL_REPORT_ATTR:
            panel_attrs(r, f, &z, 0u, &any_ok, &fail);
            return;
        case ZBZ_ZCL_WRITE_ATTR_RSP:
        case ZBZ_ZCL_CONFIG_REPORT_RSP: {
            zbz_zcl_iter_t it;
            zbz_zcl_attr_t a;
            zbz_zcl_iter(&it, &z);
            while (zbz_zcl_next_attr(&it, &a)) {
                if (a.status != ZBZ_ZCL_SUCCESS && fail == ZBZ_ZCL_SUCCESS) {
                    fail = a.status;
                }
            }
            if (q != NULL) {
                rq_end(r, q, zcl_result(fail), fail);
            }
            return;
        }
        case ZBZ_ZCL_DEFAULT_RSP: {
            uint8_t cmd = 0;
            uint8_t status = 0;
            if (zbz_zcl_decode_default_rsp(&z, &cmd, &status) != ZBZ_ZCL_OK) {
                return;
            }
            if (z.cluster == ZSRC_ZB_ZCL_IAS_WD || z.cluster == ZSRC_ZB_ZCL_IAS_ACE) {
                /* The siren's or keypad's confirmation, for its policy (radio_port.c). */
                zsrc_zb_report_t rep;
                rep_init(&rep, ZSRC_ZB_REP_ZCL, f->ieee, f->nwk, f->endpoint, 0u);
                rep.u.zcl = z;
                emit(r, &rep);
            }
            if (q != NULL) {
                rq_end(r, q, zcl_result(status), status);
            }
            return;
        }
        default: return;
        }
    }
    if (z.cluster == ZSRC_ZB_ZCL_IAS_ZONE && z.server_to_client) {
        if (z.command == ZBZ_IAS_STATUS_CHANGE) {
            zbz_ias_status_t s;
            if (zbz_ias_decode_status_change(&z, &s) == ZBZ_ZCL_OK && r->src != NULL) {
                zsrc_zb_ias_status(r->src, f->ieee, f->endpoint, s.status, r->now);
            }
        } else if (z.command == ZBZ_IAS_ENROLL_REQUEST) {
            zbz_ias_enroll_req_t e;
            if (zbz_ias_decode_enroll_request(&z, &e) == ZBZ_ZCL_OK && r->src != NULL) {
                zsrc_zb_ias_enroll_request(r->src, f->ieee, f->endpoint, e.zone_type, r->now);
            }
        }
        return;
    }
    if (z.cluster == ZSRC_ZB_ZCL_POLL_CTRL && z.server_to_client && z.command == 0x00u) {
        zsrc_zb_report_t rep;
        rep_init(&rep, ZSRC_ZB_REP_CHECK_IN, f->ieee, f->nwk, f->endpoint, 0u);
        emit(r, &rep);
        return;
    }
    if (z.cluster == ZSRC_ZB_ZCL_IAS_ACE && !z.server_to_client) {
        /* A keypad's or fob's command, raw, for the ACE policy (radio_port.c on_raw_zcl). */
        zsrc_zb_report_t rep;
        rep_init(&rep, ZSRC_ZB_REP_ZCL, f->ieee, f->nwk, f->endpoint, 0u);
        rep.u.zcl = z;
        emit(r, &rep);
    }
}

static void panel_zdo(zbf_radio_t *r, const zbf_frame_t *f)
{
    const zbf_dev_t *d = dev_of(r, f->ieee);
    const uint8_t *b = f->bytes;
    const size_t n = f->len;
    if (f->cluster == ZBF_ZDO_ANNOUNCE) {
        if (n < 12u) {
            return;
        }
        const uint16_t nwk = rd16(&b[1]);
        const uint64_t ieee = rd64(&b[3]);
        frame_seen(r, d, ieee, nwk, 0u);
        zsrc_zb_report_t rep;
        rep_init(&rep, ZSRC_ZB_REP_ANNOUNCE, ieee, nwk, 0u, 0u);
        rep.u.announce.mac_caps = b[11];
        emit(r, &rep);
        return;
    }
    if (n < 2u || (f->cluster & ZBF_ZDO_RSP) == 0u) {
        return;
    }
    const uint16_t req = (uint16_t)(f->cluster & (uint16_t)~ZBF_ZDO_RSP);
    zbf_rq_t *q = rq_match(r, f->ieee, ZBF_FRAME_ZDO, req, b[0], 0u);
    if (q == NULL) {
        return; /* the stack hands on only the answers it waits for */
    }
    const uint8_t status = b[1];
    const uint32_t tag = q->tag;
    frame_seen(r, d, f->ieee, f->nwk, 0u);
    if (status == ZBF_ZDP_SUCCESS) {
        zsrc_zb_report_t rep;
        rep_init(&rep, ZSRC_ZB_REP_NONE, f->ieee, f->nwk, 0u, tag);
        switch (req) {
        case ZBF_ZDO_NODE_DESC:
            if (n >= 17u) {
                rep.kind = ZSRC_ZB_REP_NODE_DESC;
                rep.u.node.logical_type = (uint8_t)(b[4] & 0x07u);
                rep.u.node.mac_caps = b[6];
                rep.u.node.manufacturer_code = rd16(&b[7]);
                rep.u.node.max_in_transfer = rd16(&b[10]);
                rep.u.node.server_mask = rd16(&b[12]);
            }
            break;
        case ZBF_ZDO_POWER_DESC:
            if (n >= 6u) {
                rep.kind = ZSRC_ZB_REP_POWER_DESC;
                rep.u.power.current_mode = (uint8_t)(b[4] & 0x0Fu);
                rep.u.power.available_sources = (uint8_t)(b[4] >> 4);
                rep.u.power.current_source = (uint8_t)(b[5] & 0x0Fu);
                rep.u.power.level = (uint8_t)(b[5] >> 4);
            }
            break;
        case ZBF_ZDO_ACTIVE_EP:
            if (n >= 5u && n >= 5u + (size_t)b[4]) {
                rep.kind = ZSRC_ZB_REP_ACTIVE_EP;
                for (uint8_t i = 0; i < b[4] && i < ZSRC_ZB_EP_MAX; i++) {
                    rep.u.active.ep[i] = b[5u + i];
                    rep.u.active.count++;
                }
            }
            break;
        case ZBF_ZDO_SIMPLE_DESC: {
            if (n < 13u || n < 5u + (size_t)b[4]) {
                break;
            }
            zsrc_zb_simple_desc_t *s = &rep.u.simple;
            size_t p = 5u;
            s->endpoint = b[p++];
            s->profile = rd16(&b[p]);
            p += 2u;
            s->device_id = rd16(&b[p]);
            p += 2u;
            s->version = (uint8_t)(b[p++] & 0x0Fu);
            const uint8_t in_n = b[p++];
            if (p + 2u * (size_t)in_n + 1u > n) {
                break;
            }
            for (uint8_t i = 0; i < in_n; i++, p += 2u) {
                if (s->in_count < ZSRC_ZB_EP_CLUSTERS_MAX) {
                    s->in[s->in_count++] = rd16(&b[p]);
                } else {
                    s->truncated = true;
                }
            }
            const uint8_t out_n = b[p++];
            if (p + 2u * (size_t)out_n > n) {
                break;
            }
            for (uint8_t i = 0; i < out_n; i++, p += 2u) {
                if (s->out_count < ZSRC_ZB_EP_CLUSTERS_MAX) {
                    s->out[s->out_count++] = rd16(&b[p]);
                } else {
                    s->truncated = true;
                }
            }
            rep.kind = ZSRC_ZB_REP_SIMPLE_DESC;
            rep.endpoint = s->endpoint;
            break;
        }
        default: break;
        }
        if (rep.kind != ZSRC_ZB_REP_NONE) {
            emit(r, &rep);
        }
    }
    const uint64_t ieee = q->ieee;
    rq_end(r, q, zdo_result(status), status);
    if (req == ZBF_ZDO_LEAVE && status == ZBF_ZDP_SUCCESS) {
        zsrc_zb_report_t rep;
        rep_init(&rep, ZSRC_ZB_REP_LEFT, ieee, f->nwk, 0u, tag);
        rep.u.left.rejoin = false;
        rep.u.left.asked = tag != 0u;
        emit(r, &rep);
    }
}

static void network_report(zbf_radio_t *r, uint32_t tag)
{
    zsrc_zb_report_t rep;
    rep_init(&rep, ZSRC_ZB_REP_NETWORK, 0u, 0u, 0u, tag);
    rep.u.network.channel = r->channel;
    rep.u.network.pan_id = r->pan_id;
    rep.u.network.ext_pan_id = r->ext_pan_id;
    rep.u.network.panel_ieee = r->panel_ieee;
    rep.u.network.update_id = r->update_id;
    emit(r, &rep);
}

static void local_done(zbf_radio_t *r, uint32_t tag, uint8_t result)
{
    if (tag == 0u) {
        return;
    }
    zsrc_zb_report_t rep;
    rep_init(&rep, ZSRC_ZB_REP_DONE, 0u, 0u, 0u, tag);
    rep.u.done.result = result;
    emit(r, &rep);
}

static void panel_nwk(zbf_radio_t *r, const zbf_frame_t *f)
{
    const uint8_t *b = f->bytes;
    zsrc_zb_report_t rep;
    switch (f->cluster) {
    case ZBF_NWK_JOINED:
        rep_init(&rep, ZSRC_ZB_REP_JOINED, f->ieee, f->nwk, 0u, 0u);
        rep.u.joined.join = b[0];
        rep.u.joined.rejoin = b[1] != 0u;
        rep.u.joined.parent = rd16(&b[2]);
        emit(r, &rep);
        return;
    case ZBF_NWK_REFUSED:
        rep_init(&rep, ZSRC_ZB_REP_JOIN_REFUSED, f->ieee, f->nwk, 0u, 0u);
        rep.u.refused.why = b[0];
        emit(r, &rep);
        return;
    case ZBF_NWK_LEFT:
        rep_init(&rep, ZSRC_ZB_REP_LEFT, f->ieee, f->nwk, 0u, 0u);
        rep.u.left.rejoin = b[0] != 0u;
        emit(r, &rep);
        return;
    case ZBF_NWK_ENERGY:
        if (r->src != NULL) {
            zsrc_zb_energy(r->src, (int8_t)b[0], r->now);
        }
        return;
    case ZBF_NWK_CHANNEL:
        r->channel = b[0];
        r->update_id = (uint8_t)(r->update_id + 1u);
        local_done(r, f->tag, ZSRC_ZB_DONE_OK);
        network_report(r, 0u);
        return;
    case ZBF_NWK_RESET:
        r->pan_id = (uint16_t)(r->pan_id + 1u);
        r->ext_pan_id++;
        r->update_id = 0u;
        local_done(r, f->tag, ZSRC_ZB_DONE_OK);
        network_report(r, 0u);
        return;
    case ZBF_NWK_SURVEY:
        rep_init(&rep, ZSRC_ZB_REP_SURVEY, 0u, 0u, 0u, f->tag);
        rep.u.survey.known = (uint8_t)((1u << ZSRC_ZB_CHANNELS) - 1u);
        memcpy(rep.u.survey.dbm, r->survey_dbm, sizeof(rep.u.survey.dbm));
        emit(r, &rep);
        local_done(r, f->tag, ZSRC_ZB_DONE_OK);
        return;
    default: return;
    }
}

/* ---- time -------------------------------------------------------------------------------------- */

uint64_t zbf_next_due(const zbf_radio_t *r)
{
    uint64_t t = NEVER;
    for (size_t i = 0; i < ZBF_QUEUE_MAX; i++) {
        if (r->air[i].used && r->air[i].at < t) {
            t = r->air[i].at;
        }
    }
    for (size_t i = 0; i < ZBF_RQ_MAX; i++) {
        if (r->rq[i].used && r->rq[i].deadline < t) {
            t = r->rq[i].deadline;
        }
    }
    for (size_t i = 0; i < ZBF_DEV_MAX; i++) {
        const zbf_dev_t *d = &r->dev[i];
        if (d->used && d->joined && d->check_in_s != 0u && d->next_check_in < t) {
            t = d->next_check_in;
        }
    }
    return t;
}

static void deliver(zbf_radio_t *r, zbf_air_t *slot)
{
    const zbf_air_t a = *slot;
    slot->used = false;
    if (!a.to_dev) {
        switch (a.f.kind) {
        case ZBF_FRAME_ZCL: panel_zcl(r, &a.f); break;
        case ZBF_FRAME_ZDO: panel_zdo(r, &a.f); break;
        default: panel_nwk(r, &a.f); break;
        }
        return;
    }
    zbf_dev_t *d = dev_of(r, a.f.ieee);
    if (d == NULL || !d->joined) {
        return; /* no one there: it times out */
    }
    if (!zbf_awake(r, d)) {
        const uint64_t expire = add_ms(a.sent, ZBF_HOLD_MS);
        if (r->now < expire) {
            zbf_air_t *held = air_put(r, true, expire, a.step, &a.f);
            if (held != NULL) {
                held->held = true;
                held->sent = a.sent;
            }
        }
        return; /* asleep: its parent holds it, or gave up on it */
    }
    dev_receive(r, d, &a);
}

/* One thing due at `t`: a frame first (in the order sent), then a check-in, then a request's
 * end. */
static void step_once(zbf_radio_t *r, uint64_t t)
{
    zbf_air_t *first = NULL;
    for (size_t i = 0; i < ZBF_QUEUE_MAX; i++) {
        zbf_air_t *a = &r->air[i];
        if (a->used && a->at <= t && (first == NULL || a->at < first->at ||
                                      (a->at == first->at && a->seq < first->seq))) {
            first = a;
        }
    }
    if (first != NULL) {
        deliver(r, first);
        return;
    }
    for (size_t i = 0; i < ZBF_DEV_MAX; i++) {
        zbf_dev_t *d = &r->dev[i];
        if (d->used && d->joined && d->check_in_s != 0u && d->next_check_in <= t) {
            d->next_check_in = add_ms(t, (uint64_t)d->check_in_s * 1000u);
            (void)zbf_check_in(r, d);
            return;
        }
    }
    for (size_t i = 0; i < ZBF_RQ_MAX; i++) {
        zbf_rq_t *q = &r->rq[i];
        if (q->used && q->deadline <= t) {
            rq_end(r, q, ZSRC_ZB_DONE_TIMEOUT, 0u);
            return;
        }
    }
}

void zbf_advance(zbf_radio_t *r, uint64_t now)
{
    for (unsigned guard = 0; guard < 100000u; guard++) {
        const uint64_t t = zbf_next_due(r);
        if (t == NEVER || t > now) {
            break;
        }
        if (t > r->now) {
            r->now = t;
        }
        step_once(r, t);
    }
    if (now > r->now) {
        r->now = now;
    }
}

/* ---- the hooks --------------------------------------------------------------------------------- */

static uint8_t step_of_read(uint16_t cluster)
{
    switch (cluster) {
    case ZSRC_ZB_ZCL_BASIC: return ZSRC_ZB_STEP_BASIC;
    case ZSRC_ZB_ZCL_IAS_ZONE: return ZSRC_ZB_STEP_IAS_ZONE;
    case ZSRC_ZB_ZCL_POLL_CTRL:
    case ZSRC_ZB_ZCL_IAS_WD: return ZSRC_ZB_STEP_POLL_CTRL; /* the setup reads both there */
    default: return ZSRC_ZB_STEP_NONE;
    }
}

static bool read_attrs(zbf_radio_t *r, uint64_t ieee, uint8_t ep, uint16_t cluster,
                       const uint16_t *attrs, uint8_t count, uint32_t tag)
{
    if (attrs == NULL || count == 0u || count > 8u) {
        return false;
    }
    zsrc_zb_zcl_t f;
    zbz_zcl_global(cluster, ZBZ_ZCL_READ_ATTR, false, &f);
    for (uint8_t i = 0; i < count; i++) {
        const zbz_zcl_attr_t a = {.attr = attrs[i]};
        if (!zbz_zcl_put_attr(&f, &a)) {
            return false;
        }
    }
    return send_zcl(r, ieee, ep, &f, true, ZBZ_ZCL_READ_ATTR_RSP, tag, step_of_read(cluster));
}

static bool leave_req(zbf_radio_t *r, uint64_t ieee, uint32_t tag)
{
    uint8_t b[10];
    wr64(&b[1], ieee);
    b[9] = 0x00u; /* rejoin off, children stay */
    return send_zdo(r, ieee, ZBF_ZDO_LEAVE, b, sizeof(b), tag, ZSRC_ZB_STEP_NONE);
}

static bool zdo_nwk_req(zbf_radio_t *r, uint64_t ieee, uint16_t nwk, uint16_t cluster,
                        uint32_t tag, uint8_t step)
{
    uint8_t b[3];
    wr16(&b[1], nwk);
    return send_zdo(r, ieee, cluster, b, sizeof(b), tag, step);
}

static bool h_permit_join(void *ctx, uint16_t seconds)
{
    zbf_radio_t *r = ctx;
    if (r->unplugged) {
        return false;
    }
    r->join_until = seconds != 0u ? add_ms(r->now, (uint64_t)seconds * 1000u) : 0u;
    /* Mgmt_Permit_Joining_req, broadcast: duration (0xFF would be "forever": never asked). */
    zbf_frame_t f;
    frame_init(&f, r, false, ZBF_FRAME_ZDO, 0u, 0xFFFCu, 0u, ZBF_ZDO_PERMIT_JOIN, 0u);
    const uint8_t b[3] = {r->zdo_tsn, (uint8_t)(seconds > 254u ? 254u : seconds), 0x01u};
    (void)frame_bytes(&f, b, sizeof(b));
    r->zdo_tsn = (uint8_t)(r->zdo_tsn + 1u);
    log_frame(r, &f);
    return true;
}

static bool h_cie_write(void *ctx, uint64_t ieee, uint8_t endpoint)
{
    zbf_radio_t *r = ctx;
    zsrc_zb_zcl_t f;
    zbz_ias_cie_write(r->panel_ieee, &f);
    return send_zcl(r, ieee, endpoint, &f, true, ZBZ_ZCL_WRITE_ATTR_RSP, 0u,
                    ZSRC_ZB_STEP_CIE_WRITE);
}

static bool h_enroll_response(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t zone_id)
{
    zbf_radio_t *r = ctx;
    zsrc_zb_zcl_t f;
    zbz_ias_enroll_response(ZBZ_IAS_ENROLL_OK, zone_id, &f);
    return send_zcl(r, ieee, endpoint, &f, false, ZBZ_ZCL_DEFAULT_RSP, 0u, ZSRC_ZB_STEP_ENROLL);
}

static bool h_read_attribute(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                             uint16_t attr)
{
    return read_attrs(ctx, ieee, endpoint, cluster, &attr, 1u, 0u);
}

static bool h_remove_device(void *ctx, uint64_t ieee)
{
    return leave_req(ctx, ieee, 0u);
}

static bool h_energy_scan(void *ctx)
{
    zbf_radio_t *r = ctx;
    const uint8_t b[1] = {(uint8_t)r->energy_dbm};
    return local_signal(r, ZBF_NWK_ENERGY, b, sizeof(b), 0u);
}

static bool h_node_desc(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    return zdo_nwk_req(ctx, ieee, nwk, ZBF_ZDO_NODE_DESC, tag, ZSRC_ZB_STEP_NODE_DESC);
}

static bool h_power_desc(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    return zdo_nwk_req(ctx, ieee, nwk, ZBF_ZDO_POWER_DESC, tag, ZSRC_ZB_STEP_POWER_DESC);
}

static bool h_active_ep(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    return zdo_nwk_req(ctx, ieee, nwk, ZBF_ZDO_ACTIVE_EP, tag, ZSRC_ZB_STEP_ACTIVE_EP);
}

static bool h_simple_desc(void *ctx, uint64_t ieee, uint16_t nwk, uint8_t endpoint, uint32_t tag)
{
    uint8_t b[4];
    wr16(&b[1], nwk);
    b[3] = endpoint;
    return send_zdo(ctx, ieee, ZBF_ZDO_SIMPLE_DESC, b, sizeof(b), tag, ZSRC_ZB_STEP_SIMPLE_DESC);
}

static bool h_read_attributes(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                              const uint16_t *attrs, uint8_t count, uint32_t tag)
{
    return read_attrs(ctx, ieee, endpoint, cluster, attrs, count, tag);
}

static bool h_bind(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint32_t tag)
{
    zbf_radio_t *r = ctx;
    uint8_t b[22];
    wr64(&b[1], ieee);
    b[9] = endpoint;
    wr16(&b[10], cluster);
    b[12] = 0x03u; /* a 64-bit destination address and endpoint */
    wr64(&b[13], r->panel_ieee);
    b[21] = ZBF_PANEL_EP;
    return send_zdo(r, ieee, ZBF_ZDO_BIND, b, sizeof(b), tag, ZSRC_ZB_STEP_BIND);
}

static bool h_configure_reporting(void *ctx, uint64_t ieee, uint8_t endpoint,
                                  const zsrc_zb_reporting_t *rep, uint32_t tag)
{
    zsrc_zb_zcl_t f;
    if (rep == NULL || !zbz_zcl_config_report(rep, 1u, &f)) {
        return false;
    }
    return send_zcl(ctx, ieee, endpoint, &f, true, ZBZ_ZCL_CONFIG_REPORT_RSP, tag,
                    ZSRC_ZB_STEP_REPORTING);
}

static bool h_leave(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag)
{
    (void)nwk;
    return leave_req(ctx, ieee, tag);
}

static void code_drop(zbf_radio_t *r, uint64_t ieee)
{
    for (size_t i = 0; i < ZBF_CODES_MAX; i++) {
        if (r->codes[i].used && r->codes[i].ieee == ieee) {
            memset(&r->codes[i], 0, sizeof(r->codes[i]));
        }
    }
}

static bool h_forget(void *ctx, uint64_t ieee)
{
    zbf_radio_t *r = ctx;
    if (r->unplugged) {
        return false;
    }
    zbf_dev_t *d = dev_of(r, ieee);
    if (d != NULL) {
        d->joined = false; /* out of the stack's tables: its frames are no longer the network's */
    }
    code_drop(r, ieee);
    return true;
}

static bool h_identify(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t seconds, uint32_t tag)
{
    zsrc_zb_zcl_t f;
    memset(&f, 0, sizeof(f));
    f.cluster = ZSRC_ZB_ZCL_IDENTIFY;
    f.command = 0x00u; /* Identify: identify time u16 */
    f.want_response = true;
    f.tsn = ZSRC_ZB_TSN_NEW;
    wr16(f.payload, seconds);
    f.len = 2;
    return send_zcl(ctx, ieee, endpoint, &f, false, ZBZ_ZCL_DEFAULT_RSP, tag, ZSRC_ZB_STEP_NONE);
}

static bool h_ias_test_mode(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t seconds,
                            uint32_t tag)
{
    zsrc_zb_zcl_t f;
    zbz_ias_test_mode(seconds, 0u, &f);
    return send_zcl(ctx, ieee, endpoint, &f, false, ZBZ_ZCL_DEFAULT_RSP, tag, ZSRC_ZB_STEP_NONE);
}

static bool h_zcl_command(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                          uint32_t tag)
{
    if (cmd == NULL) {
        return false;
    }
    return send_zcl(ctx, ieee, endpoint, cmd, false, ZBZ_ZCL_DEFAULT_RSP, tag, ZSRC_ZB_STEP_NONE);
}

static bool code_len_ok(uint8_t len)
{
    return len == 8u || len == 10u || len == 14u || len == 18u;
}

static bool h_install_code(void *ctx, uint64_t ieee, const uint8_t *code, uint8_t len)
{
    zbf_radio_t *r = ctx;
    if (r->unplugged || ieee == 0u) {
        return false;
    }
    code_drop(r, ieee);
    if (len == 0u) {
        return true;
    }
    if (code == NULL || !code_len_ok(len)) {
        return false;
    }
    for (size_t i = 0; i < ZBF_CODES_MAX; i++) {
        if (!r->codes[i].used) {
            r->codes[i].used = true;
            r->codes[i].ieee = ieee;
            r->codes[i].len = len;
            memcpy(r->codes[i].code, code, len);
            return true;
        }
    }
    return false;
}

static bool h_codes_only(void *ctx, bool on)
{
    zbf_radio_t *r = ctx;
    if (r->unplugged) {
        return false;
    }
    r->codes_only = on;
    return true;
}

static bool h_set_channel(void *ctx, uint8_t channel, uint32_t tag)
{
    zbf_radio_t *r = ctx;
    if (r->unplugged || (channel != 11u && channel != 15u && channel != 20u && channel != 25u)) {
        return false;
    }
    /* Mgmt_NWK_Update_req to every device: the channel mask, 0xFE (change channel), the update
     * id the network moves to. */
    zbf_frame_t f;
    frame_init(&f, r, false, ZBF_FRAME_ZDO, 0u, 0xFFFDu, 0u, ZBF_ZDO_NWK_UPDATE, tag);
    uint8_t b[7];
    b[0] = r->zdo_tsn;
    wr32(&b[1], (uint32_t)1u << channel);
    b[5] = 0xFEu;
    b[6] = (uint8_t)(r->update_id + 1u);
    (void)frame_bytes(&f, b, sizeof(b));
    r->zdo_tsn = (uint8_t)(r->zdo_tsn + 1u);
    log_frame(r, &f);
    const uint8_t s[1] = {channel};
    return local_signal(r, ZBF_NWK_CHANNEL, s, sizeof(s), tag);
}

static bool h_reset_network(void *ctx, uint32_t tag)
{
    zbf_radio_t *r = ctx;
    if (r->unplugged) {
        return false;
    }
    for (size_t i = 0; i < ZBF_DEV_MAX; i++) {
        r->dev[i].joined = false; /* every device went with the old network */
    }
    memset(r->codes, 0, sizeof(r->codes));
    r->codes_only = false;
    r->join_until = 0u;
    return local_signal(r, ZBF_NWK_RESET, NULL, 0u, tag);
}

static bool h_channel_survey(void *ctx, uint32_t tag)
{
    zbf_radio_t *r = ctx;
    if (r->join_until > r->now) {
        return false; /* not while devices are joining */
    }
    return local_signal(r, ZBF_NWK_SURVEY, NULL, 0u, tag);
}

zsrc_zb_radio_t zbf_hooks(zbf_radio_t *r)
{
    zsrc_zb_radio_t h;
    memset(&h, 0, sizeof(h));
    h.ctx = r;
    h.permit_join = h_permit_join;
    h.ias_cie_write = h_cie_write;
    h.ias_enroll_response = h_enroll_response;
    h.read_attribute = h_read_attribute;
    h.remove_device = h_remove_device;
    h.energy_scan = h_energy_scan;
    h.node_desc_req = h_node_desc;
    h.power_desc_req = h_power_desc;
    h.active_ep_req = h_active_ep;
    h.simple_desc_req = h_simple_desc;
    h.read_attributes = h_read_attributes;
    h.bind = h_bind;
    h.configure_reporting = h_configure_reporting;
    h.leave = h_leave;
    h.forget = h_forget;
    h.identify = h_identify;
    h.ias_test_mode = h_ias_test_mode;
    h.zcl_command = h_zcl_command;
    h.install_code = h_install_code;
    h.codes_only = h_codes_only;
    h.set_channel = h_set_channel;
    h.reset_network = h_reset_network;
    h.channel_survey = h_channel_survey;
    return h;
}

/* ---- the radio --------------------------------------------------------------------------------- */

void zbf_init(zbf_radio_t *r, zsrc_zb_t *src, const zbf_sink_t *sink, uint64_t now)
{
    memset(r, 0, sizeof(*r));
    r->src = src;
    if (sink != NULL) {
        r->sink = *sink;
    }
    r->now = now;
    r->panel_ieee = ZBF_PANEL_IEEE;
    r->pan_id = 0x1A2Bu;
    r->ext_pan_id = 0x02ABCD00000000F1ull;
    r->channel = 15u;
    r->tsn = 1u;
    r->zdo_tsn = 1u;
    r->energy_dbm = -85;
    for (size_t i = 0; i < ZSRC_ZB_CHANNELS; i++) {
        r->survey_dbm[i] = (int8_t)(-90 + (int)i);
    }
}

void zbf_up(zbf_radio_t *r)
{
    r->up = true;
    network_report(r, 0u);
    if (r->src != NULL) {
        zsrc_zb_link(r->src, true, r->now);
    }
}

void zbf_unplug(zbf_radio_t *r, bool unplugged)
{
    if (!unplugged) {
        r->unplugged = false;
        zbf_up(r);
        return;
    }
    r->unplugged = true;
    r->up = false;
    r->join_until = 0u;
    memset(r->air, 0, sizeof(r->air));
    for (size_t i = 0; i < ZBF_RQ_MAX; i++) {
        if (r->rq[i].used) {
            rq_end(r, &r->rq[i], ZSRC_ZB_DONE_NO_RADIO, 0u);
        }
    }
    if (r->src != NULL) {
        zsrc_zb_link(r->src, false, r->now);
    }
}

void zbf_set_tsn(zbf_radio_t *r, uint8_t zcl, uint8_t zdo)
{
    r->tsn = zcl;
    r->zdo_tsn = zdo;
}

void zbf_set_energy(zbf_radio_t *r, int8_t dbm)
{
    r->energy_dbm = dbm;
}

size_t zbf_log_count(const zbf_radio_t *r)
{
    return r->log_len;
}

const zbf_frame_t *zbf_log_at(const zbf_radio_t *r, size_t i)
{
    if (i >= r->log_len) {
        return NULL;
    }
    return &r->log[(r->log_head + i) % ZBF_LOG_MAX];
}

void zbf_log_clear(zbf_radio_t *r)
{
    r->log_head = 0u;
    r->log_len = 0u;
}

const zbf_frame_t *zbf_last_tx(const zbf_radio_t *r, uint64_t ieee)
{
    for (size_t i = r->log_len; i > 0u; i--) {
        const zbf_frame_t *f = zbf_log_at(r, i - 1u);
        if (!f->rx && (ieee == 0u || f->ieee == ieee)) {
            return f;
        }
    }
    return NULL;
}

/* ---- the signatures and the devices ------------------------------------------------------------ */

size_t zbf_signature_count(void)
{
    return ZB_SIGNATURE_COUNT;
}

const char *zbf_signature_file(size_t i)
{
    return i < ZB_SIGNATURE_COUNT ? ZB_SIGNATURES[i].file : NULL;
}

const char *zbf_signature_device(size_t i)
{
    return i < ZB_SIGNATURE_COUNT ? ZB_SIGNATURES[i].device : NULL;
}

static bool name_is(const zbsig_t *s, const char *name)
{
    if (strcmp(name, s->file) == 0 || strcmp(name, s->device) == 0) {
        return true;
    }
    const size_t n = strlen(name);
    return strncmp(name, s->file, n) == 0 && strcmp(&s->file[n], ".json") == 0;
}

zbf_dev_t *zbf_add_row(zbf_radio_t *r, size_t row)
{
    if (row >= ZB_SIGNATURE_COUNT) {
        return NULL;
    }
    const zbsig_t *s = &ZB_SIGNATURES[row];
    if (dev_of(r, s->ieee) != NULL) {
        return NULL;
    }
    zbf_dev_t *d = NULL;
    for (size_t i = 0; i < ZBF_DEV_MAX && d == NULL; i++) {
        if (!r->dev[i].used) {
            d = &r->dev[i];
        }
    }
    if (d == NULL) {
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->sig = (uint16_t)row;
    d->ieee = s->ieee;
    d->nwk = s->nwk;
    d->node = s->node;
    d->power_source = s->power_source;
    strncpy(d->manufacturer, s->manufacturer, sizeof(d->manufacturer) - 1u);
    strncpy(d->model, s->model, sizeof(d->model) - 1u);
    d->ep_count = (s->ep_count > ZSRC_ZB_EP_MAX) ? (uint8_t)ZSRC_ZB_EP_MAX : s->ep_count;
    for (uint8_t i = 0; i < d->ep_count; i++) {
        const zbsig_ep_t *se = &s->ep[i];
        zbf_ep_t *e = &d->ep[i];
        e->sd.endpoint = se->endpoint;
        e->sd.profile = se->profile;
        e->sd.device_id = se->device_id;
        e->sd.in_count = se->in_count;
        e->sd.out_count = se->out_count;
        memcpy(e->sd.in, se->in, sizeof(e->sd.in));
        memcpy(e->sd.out, se->out, sizeof(e->sd.out));
        e->zone_type = se->zone_type;
        e->zone_id = 0xFFu;
        if (se->zone_type != ZSRC_ZB_ZONE_TYPE_NONE) {
            d->auto_enroll = true;
        }
    }
    d->sleepy = s->sleepy;
    d->wake_ms = ZBF_WAKE_MS;
    const bool mains = (s->power_source & 0x7Fu) == 0x01u || (s->power_source & 0x7Fu) == 0x04u;
    d->battery_pct = mains ? ZSRC_ZB_BATTERY_UNKNOWN : 100u;
    d->lqi = 200u;
    d->rssi = -55;
    d->tsn = (uint8_t)(0x20u + row);
    return d;
}

zbf_dev_t *zbf_add(zbf_radio_t *r, const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ZB_SIGNATURE_COUNT; i++) {
        if (name_is(&ZB_SIGNATURES[i], name)) {
            return zbf_add_row(r, i);
        }
    }
    return NULL;
}

zbf_dev_t *zbf_find(zbf_radio_t *r, uint64_t ieee)
{
    return dev_of(r, ieee);
}

void zbf_set_code(zbf_dev_t *d, const uint8_t *code, uint8_t len)
{
    memset(d->code, 0, sizeof(d->code));
    d->code_len = 0u;
    if (code != NULL && code_len_ok(len)) {
        memcpy(d->code, code, len);
        d->code_len = len;
    }
}

static bool tc_holds(const zbf_radio_t *r, const zbf_dev_t *d)
{
    for (size_t i = 0; i < ZBF_CODES_MAX; i++) {
        if (r->codes[i].used && r->codes[i].ieee == d->ieee && r->codes[i].len == d->code_len &&
            memcmp(r->codes[i].code, d->code, d->code_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool dev_joined(zbf_radio_t *r, zbf_dev_t *d, bool rejoin)
{
    d->joined = true;
    uint8_t b[4] = {d->join, (uint8_t)(rejoin ? 1u : 0u), 0u, 0u};
    wr16(&b[2], 0x0000u); /* through the panel itself */
    if (!dev_signal(r, d, ZBF_NWK_JOINED, b, sizeof(b))) {
        return false;
    }
    dev_spoke(r, d);
    if (d->check_in_s != 0u) {
        d->next_check_in = add_ms(r->now, (uint64_t)d->check_in_s * 1000u);
    }
    return dev_announce(r, d);
}

bool zbf_join(zbf_radio_t *r, zbf_dev_t *d)
{
    if (d == NULL || r->unplugged || !r->up || d->joined || r->now >= r->join_until) {
        return false;
    }
    if (d->code_len != 0u && tc_holds(r, d)) {
        d->join = ZSRC_ZB_JOIN_INSTALL_CODE;
    } else if (r->codes_only) {
        const uint8_t b[1] = {1u}; /* no install code matched while codes are required */
        (void)dev_signal(r, d, ZBF_NWK_REFUSED, b, sizeof(b));
        return false;
    } else {
        d->join = ZSRC_ZB_JOIN_STANDARD_KEY;
    }
    return dev_joined(r, d, false);
}

bool zbf_rejoin(zbf_radio_t *r, zbf_dev_t *d, uint16_t nwk)
{
    if (d == NULL || r->unplugged || !r->up) {
        return false;
    }
    if (nwk != 0u) {
        d->nwk = nwk;
    }
    return dev_joined(r, d, true);
}

void zbf_leave(zbf_radio_t *r, zbf_dev_t *d, bool rejoin)
{
    if (d == NULL || !d->joined) {
        return;
    }
    d->joined = false;
    const uint8_t b[1] = {(uint8_t)(rejoin ? 1u : 0u)};
    (void)dev_signal(r, d, ZBF_NWK_LEFT, b, sizeof(b));
}

bool zbf_check_in(zbf_radio_t *r, zbf_dev_t *d)
{
    if (d == NULL || !d->joined) {
        return false;
    }
    const zbf_ep_t *e = ep_with(d, ZSRC_ZB_ZCL_POLL_CTRL);
    if (e == NULL) {
        return dev_announce(r, d); /* a button press: it announces itself */
    }
    zsrc_zb_zcl_t z;
    memset(&z, 0, sizeof(z));
    z.cluster = ZSRC_ZB_ZCL_POLL_CTRL;
    z.command = 0x00u; /* Check-in */
    z.server_to_client = true;
    z.want_response = true;
    z.tsn = ZSRC_ZB_TSN_NEW;
    return dev_send_zcl(r, d, e->sd.endpoint, &z, false, 0u, NULL);
}

static bool send_status(zbf_radio_t *r, zbf_dev_t *d, zbf_ep_t *e)
{
    zsrc_zb_zcl_t z;
    memset(&z, 0, sizeof(z));
    z.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    z.command = ZBZ_IAS_STATUS_CHANGE;
    z.server_to_client = true;
    z.tsn = ZSRC_ZB_TSN_NEW;
    wr16(&z.payload[0], ias_word(r, e));
    z.payload[2] = 0u;          /* extended status */
    z.payload[3] = e->zone_id;
    wr16(&z.payload[4], 0u);    /* delay */
    z.len = 6;
    return dev_send_zcl(r, d, e->sd.endpoint, &z, false, 0u, NULL);
}

static bool send_report(zbf_radio_t *r, zbf_dev_t *d, zbf_ep_t *e, uint16_t cluster, uint16_t attr)
{
    attr_val_t v;
    if (!dev_attr(r, d, e, cluster, attr, &v)) {
        return false;
    }
    zsrc_zb_zcl_t z;
    zbz_zcl_global(cluster, ZBZ_ZCL_REPORT_ATTR, true, &z);
    z.want_response = false;
    const zbz_zcl_attr_t a = {.attr = attr, .type = v.type, .len = v.len, .value = v.value};
    if (!zbz_zcl_put_attr(&z, &a)) {
        return false;
    }
    return dev_send_zcl(r, d, e->sd.endpoint, &z, true, 0u, NULL);
}

bool zbf_ias_status(zbf_radio_t *r, zbf_dev_t *d, uint8_t endpoint, uint16_t status)
{
    zbf_ep_t *e = zbf_ep(d, endpoint);
    if (d == NULL || !d->joined || e == NULL || !has_in(e, ZSRC_ZB_ZCL_IAS_ZONE)) {
        return false;
    }
    e->status = (uint16_t)(status & (uint16_t)~ZSRC_ZB_IAS_TEST);
    return send_status(r, d, e);
}

bool zbf_trip(zbf_radio_t *r, zbf_dev_t *d, bool on)
{
    if (d == NULL || !d->joined) {
        return false;
    }
    zbf_ep_t *e = ep_with(d, ZSRC_ZB_ZCL_IAS_ZONE);
    if (e != NULL) {
        e->status = on ? (uint16_t)(e->status | ZSRC_ZB_IAS_ALARM1)
                       : (uint16_t)(e->status & (uint16_t)~ZSRC_ZB_IAS_ALARM1);
        return send_status(r, d, e);
    }
    const uint16_t kinds[2] = {ZSRC_ZB_ZCL_OCCUPANCY, ZSRC_ZB_ZCL_ON_OFF};
    for (size_t k = 0; k < 2u; k++) {
        e = ep_with(d, kinds[k]);
        if (e != NULL) {
            e->on = on;
            return send_report(r, d, e, kinds[k], 0x0000u);
        }
    }
    return false;
}

bool zbf_set_battery(zbf_radio_t *r, zbf_dev_t *d, uint8_t percent)
{
    if (d == NULL) {
        return false;
    }
    d->battery_pct = (percent > 100u && percent != ZSRC_ZB_BATTERY_UNKNOWN) ? 100u : percent;
    zbf_ep_t *e = ep_with(d, ZSRC_ZB_ZCL_POWER_CFG);
    if (!d->joined || e == NULL) {
        return false;
    }
    return send_report(r, d, e, ZSRC_ZB_ZCL_POWER_CFG, 0x0021u);
}

bool zbf_set_temperature(zbf_radio_t *r, zbf_dev_t *d, int16_t value_x100)
{
    zbf_ep_t *e = (d != NULL) ? ep_with(d, ZSRC_ZB_ZCL_TEMP_MEAS) : NULL;
    if (e == NULL) {
        return false;
    }
    e->temp_x100 = value_x100;
    if (!d->joined) {
        return false;
    }
    return send_report(r, d, e, ZSRC_ZB_ZCL_TEMP_MEAS, 0x0000u);
}

bool zbf_send(zbf_radio_t *r, zbf_dev_t *d, uint8_t endpoint, const zsrc_zb_zcl_t *cmd)
{
    if (d == NULL || cmd == NULL || !d->joined || zbf_ep(d, endpoint) == NULL) {
        return false;
    }
    return dev_send_zcl(r, d, endpoint, cmd, false, 0u, NULL);
}

bool zbf_repeat(zbf_radio_t *r, zbf_dev_t *d)
{
    if (d == NULL || !d->has_last || !d->joined) {
        return false;
    }
    zbf_frame_t f = d->last;
    f.at = r->now;
    f.tag = 0u;
    return dev_put(r, d, &f, 0u);
}

void zbf_set_link(zbf_dev_t *d, uint8_t lqi, int8_t rssi)
{
    d->lqi = lqi;
    d->rssi = rssi;
}

void zbf_set_sleepy(zbf_dev_t *d, bool sleepy, uint32_t wake_ms)
{
    d->sleepy = sleepy;
    d->wake_ms = wake_ms;
    d->awake_until = 0u;
}

void zbf_set_check_in(zbf_radio_t *r, zbf_dev_t *d, uint32_t seconds)
{
    d->check_in_s = seconds;
    d->next_check_in = seconds != 0u ? add_ms(r->now, (uint64_t)seconds * 1000u) : 0u;
}

void zbf_fault(zbf_dev_t *d, uint8_t step, const zbf_fault_t *f)
{
    if (d == NULL || step > ZSRC_ZB_STEP_DONE) {
        return;
    }
    if (f == NULL) {
        memset(&d->fault[step], 0, sizeof(d->fault[step]));
    } else {
        d->fault[step] = *f;
    }
}
