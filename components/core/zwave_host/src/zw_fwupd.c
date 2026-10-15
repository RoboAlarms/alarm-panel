/*
 * zw_fwupd.c - sending a device its new firmware. See zw_fwupd.h.
 */
#include "zwave_host/zw_fwupd.h"

#include <string.h>

#define CMD_MAX 64u

static void end(zw_fwupd_t *f, zw_fwupd_result_t r, uint8_t detail)
{
    f->waiting_wake = false;
    f->asked = false;
    f->state = ZW_FWUPD_IDLE;
    f->result = r;
    f->detail = detail;
    f->deadline = UINT64_MAX;
    f->next = 0u;
    f->out = false;
    f->seq = (uint16_t)((unsigned)f->seq + 1u); /* a send still out is no longer ours */
}

static uint32_t tag_of(const zw_fwupd_t *f)
{
    return ZW_FWUPD_TAG_BASE | (uint32_t)f->seq;
}

static bool send(zw_fwupd_t *f, const uint8_t *cmd, size_t len)
{
    f->seq = (uint16_t)((unsigned)f->seq + 1u);
    f->out = true;
    if (len == 0u || f->port.send == NULL || !f->port.send(f->port.ctx, f->node, cmd, len,
                                                           tag_of(f))) {
        f->out = false;
        return false;
    }
    return true;
}

/* The command a state sends (again, on a time-out or a failed send). */
static bool send_state(zw_fwupd_t *f)
{
    uint8_t cmd[CMD_MAX];
    size_t n = 0u;
    switch (f->state) {
    case ZW_FWUPD_MD: n = zw_fwu_md_get_build(cmd, sizeof cmd); break;
    case ZW_FWUPD_REQUEST: {
        const zw_fwu_request_t q = {
            .manufacturer_id = f->md.manufacturer_id,
            .firmware_id = f->target == 0u ? f->md.firmware_id : f->md.target_ids[(unsigned)f->target - 1u],
            .checksum = f->checksum,
            .target = f->target,
            .fragment_size = f->fragment,
            .activation = f->md.supports_activation,
            .hardware = (uint8_t)(f->md.version_seen >= 5u ? (unsigned)f->md.hardware : 0xFFu),
            .cc_version = f->cc_version};
        n = zw_fwu_request_get_build(cmd, sizeof cmd, &q);
        break;
    }
    case ZW_FWUPD_ACTIVATE: {
        const zw_fwu_activation_t a = {
            f->md.manufacturer_id,
            f->target == 0u ? f->md.firmware_id : f->md.target_ids[(unsigned)f->target - 1u], f->checksum,
            f->target, 0u,
            (uint8_t)(f->md.version_seen >= 5u ? (unsigned)f->md.hardware : 0xFFu)};
        n = zw_fwu_activation_set_build(cmd, sizeof cmd, &a);
        break;
    }
    default: return false;
    }
    f->sending = 0u; /* not a fragment: its end moves no fragment on */
    return send(f, cmd, n);
}

/* The next fragment of the running Get, one send at a time. */
static void pump(zw_fwupd_t *f)
{
    if (f->out || f->next == 0u || f->next > f->until) {
        return;
    }
    const size_t at = ((size_t)f->next - 1u) * (size_t)f->fragment;
    const size_t left = f->len - at;
    const size_t n = left < (size_t)f->fragment ? left : (size_t)f->fragment;
    uint8_t cmd[CMD_MAX + 8u];
    const size_t len = zw_fwu_report_build(cmd, sizeof cmd, f->next, f->next == f->total,
                                           &f->image[at], n, f->cc_version >= 2u);
    f->sending = f->next;
    if (!send(f, cmd, len)) {
        if (++f->tries >= ZW_FWUPD_TRIES) {
            end(f, ZW_FWUPD_SEND_FAILED, 0u);
        }
    }
}

void zw_fwupd_init(zw_fwupd_t *f, const zw_fwupd_port_t *port)
{
    memset(f, 0, sizeof(*f));
    if (port != NULL) {
        f->port = *port;
    }
    f->deadline = UINT64_MAX;
}

bool zw_fwupd_start(zw_fwupd_t *f, uint64_t now, uint16_t node, uint8_t cc_version,
                    uint16_t expect_manufacturer, uint8_t target, const uint8_t *image,
                    size_t len, size_t max_payload)
{
    f->now = now;
    const size_t overhead = 4u + (cc_version >= 2u ? 2u : 0u);
    if (f->state != ZW_FWUPD_IDLE || image == NULL || len == 0u || node == 0u ||
        max_payload <= overhead || max_payload > CMD_MAX) {
        return false;
    }
    f->node = node;
    f->cc_version = (uint8_t)(cc_version != 0u ? (unsigned)cc_version : 1u);
    f->expect_manufacturer = expect_manufacturer;
    f->target = target;
    f->image = image;
    f->len = len;
    f->checksum = zw_fwu_image_checksum(image, len);
    f->fragment = (uint16_t)(max_payload - overhead);
    f->total = 0u;
    f->sent_max = 0u;
    f->sending = 0u;
    f->asked = false;
    f->next = 0u;
    f->result = ZW_FWUPD_RUNNING;
    f->detail = 0u;
    f->wait_s = 0u;
    memset(&f->md, 0, sizeof f->md);
    f->state = ZW_FWUPD_MD;
    f->tries = 1u;
    f->waiting_wake = f->wake_wait_ms != 0u;
    f->deadline = now + (f->waiting_wake ? f->wake_wait_ms : ZW_FWUPD_MD_MS);
    f->wake_wait_ms = 0u;
    if (!send_state(f)) {
        f->out = false; /* the time-out asks again */
    }
    return true;
}

static void on_md(zw_fwupd_t *f, const uint8_t *cmd, size_t len)
{
    zw_fwu_md_t md;
    if (!zw_fwu_md_report_parse(cmd, len, &md)) {
        return;
    }
    if (f->expect_manufacturer != 0u && md.manufacturer_id != f->expect_manufacturer) {
        end(f, ZW_FWUPD_WRONG_DEVICE, 0u);
        return;
    }
    if (f->target > 0u && (f->target > md.target_count || f->target > ZW_FWU_TARGETS_MAX)) {
        end(f, ZW_FWUPD_WRONG_DEVICE, 0u);
        return;
    }
    if (f->target == 0u && !md.upgradable) {
        end(f, ZW_FWUPD_NOT_UPGRADABLE, 0u);
        return;
    }
    f->md = md;
    f->waiting_wake = false; /* it answered: awake, whatever became of the send's own end */
    if (f->cc_version >= 3u && md.max_fragment != 0u && md.max_fragment < f->fragment) {
        f->fragment = md.max_fragment;
    }
    const size_t total = (f->len + (size_t)f->fragment - 1u) / (size_t)f->fragment;
    if (total > 0x7FFFu) {
        end(f, ZW_FWUPD_REFUSED, ZW_FWU_REQ_FRAGMENT_TOO_BIG); /* more fragments than it counts */
        return;
    }
    f->total = (uint16_t)total;
    f->state = ZW_FWUPD_REQUEST;
    f->tries = 1u;
    f->deadline = f->now + ZW_FWUPD_MD_MS;
    (void)send_state(f);
}

static void on_status(zw_fwupd_t *f, const uint8_t *cmd, size_t len)
{
    uint8_t st = 0u;
    bool has_wait = false;
    uint16_t wait = 0u;
    if (!zw_fwu_status_report_parse(cmd, len, &st, &has_wait, &wait)) {
        return;
    }
    if (st == ZW_FWU_STATUS_OK_RESTART) {
        end(f, ZW_FWUPD_OK_RESTART, st);
        f->wait_s = wait;
    } else if (st == ZW_FWU_STATUS_OK_NO_RESTART) {
        end(f, ZW_FWUPD_OK_NO_RESTART, st);
    } else if (st == ZW_FWU_STATUS_WAIT_ACTIVATE && f->md.supports_activation) {
        f->state = ZW_FWUPD_ACTIVATE;
        f->tries = 1u;
        f->deadline = f->now + ZW_FWUPD_ACTIVATE_MS;
        (void)send_state(f);
    } else if (st == ZW_FWU_STATUS_WAIT_ACTIVATE) {
        end(f, ZW_FWUPD_DEVICE_FAILED, st); /* it waits for an activation it never offered */
    } else {
        end(f, ZW_FWUPD_DEVICE_FAILED, st);
    }
}

void zw_fwupd_expect_sleep(zw_fwupd_t *f, uint32_t ms)
{
    f->wake_wait_ms = ms;
}

bool zw_fwupd_received(zw_fwupd_t *f, uint64_t now, uint16_t node, const uint8_t *cmd,
                       size_t len)
{
    f->now = now;
    if (f->state == ZW_FWUPD_IDLE || node != f->node || cmd == NULL || len < 2u ||
        cmd[0] != ZW_CC_FIRMWARE_UPDATE) {
        return false;
    }
    switch (cmd[1]) {
    case ZW_FWU_MD_REPORT:
        if (f->state == ZW_FWUPD_MD) {
            on_md(f, cmd, len);
        }
        break;
    case ZW_FWU_REQUEST_REPORT: {
        uint8_t st = 0u;
        if (f->state == ZW_FWUPD_REQUEST && zw_fwu_request_report_parse(cmd, len, &st)) {
            if (st == ZW_FWU_REQ_OK) {
                f->state = ZW_FWUPD_TRANSFER;
                f->tries = 0u;
                f->deadline = now + ZW_FWUPD_GET_MS;
            } else {
                end(f, ZW_FWUPD_REFUSED, st);
            }
        }
        break;
    }
    case ZW_FWU_GET: {
        zw_fwu_get_t g;
        if ((f->state == ZW_FWUPD_TRANSFER || f->state == ZW_FWUPD_STATUS) &&
            zw_fwu_get_parse(cmd, len, &g) && g.report_number <= f->total) {
            f->state = ZW_FWUPD_TRANSFER;
            f->asked = f->out; /* the send on its way doesn't move `next` on when it ends */
            f->next = g.report_number;
            const uint32_t until = (uint32_t)g.report_number + (uint32_t)g.reports - 1u;
            f->until = (uint16_t)(until < (uint32_t)f->total ? until : (uint32_t)f->total);
            f->tries = 0u;
            f->deadline = now + ZW_FWUPD_GET_MS;
            pump(f);
        }
        break;
    }
    case ZW_FWU_STATUS_REPORT:
        if (f->state == ZW_FWUPD_TRANSFER || f->state == ZW_FWUPD_STATUS) {
            on_status(f, cmd, len);
        }
        break;
    case ZW_FWU_ACTIVATION_REPORT: {
        zw_fwu_activation_t a;
        if (f->state == ZW_FWUPD_ACTIVATE && zw_fwu_activation_report_parse(cmd, len, &a)) {
            end(f, a.status == 0xFFu ? ZW_FWUPD_OK_RESTART : ZW_FWUPD_ACTIVATION_FAILED, a.status);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

bool zw_fwupd_send_done(zw_fwupd_t *f, uint64_t now, uint32_t tag, bool delivered)
{
    f->now = now;
    if ((tag & ZW_FWUPD_TAG_MASK) != ZW_FWUPD_TAG_BASE) {
        return false;
    }
    if (tag != tag_of(f) || !f->out) {
        return true; /* an earlier update's, or an earlier try's */
    }
    f->out = false;
    if (f->waiting_wake && f->state == ZW_FWUPD_MD && delivered) {
        f->waiting_wake = false; /* it woke and has the MD Get: its answer's wait starts */
        f->deadline = now + ZW_FWUPD_MD_MS;
        return true;
    }
    if (f->state == ZW_FWUPD_TRANSFER) {
        if (!delivered) {
            if (++f->tries >= ZW_FWUPD_TRIES) {
                end(f, ZW_FWUPD_SEND_FAILED, 0u);
                return true;
            }
        } else {
            /* the fragment this send carried. The device's next Get may have come before its end
             * (its radio answers faster than the module reports): that Get decides what follows -
             * the next ones, none skipped, or this one again when the device lost it. */
            const uint16_t sent = f->sending;
            const bool asked = f->asked;
            f->asked = false;
            f->tries = 0u;
            if (sent == 0u) {
                pump(f); /* the Request Get's own end, after the device's answer and first Get */
                return true;
            }
            if (sent > f->sent_max) {
                f->sent_max = sent;
            }
            /* A fragment delivered is progress, not silence: the wait for the device's next Get
             * runs from the last one delivered, so it starts once the batch is out, however long
             * a large batch takes on a slow link (Z-Wave JS waits for the next request only
             * after sending the batch). A send that never ends moves nothing: still bounded. */
            f->deadline = now + ZW_FWUPD_GET_MS;
            if (!asked) {
                if (sent == f->total) {
                    f->next = 0u; /* the last one: its verdict */
                    f->state = ZW_FWUPD_STATUS;
                    f->deadline = now + ZW_FWUPD_STATUS_MS;
                    return true;
                }
                f->next++;
            }
        }
        pump(f);
        return true;
    }
    if (!delivered && (f->state == ZW_FWUPD_MD || f->state == ZW_FWUPD_REQUEST ||
                       f->state == ZW_FWUPD_ACTIVATE)) {
        if (++f->tries > ZW_FWUPD_TRIES) {
            end(f, ZW_FWUPD_SEND_FAILED, 0u);
        } else {
            (void)send_state(f);
        }
    }
    return true;
}

void zw_fwupd_poll(zw_fwupd_t *f, uint64_t now)
{
    f->now = now;
    if (f->state == ZW_FWUPD_TRANSFER && !f->out && f->next != 0u && f->next <= f->until) {
        pump(f); /* a fragment that could not even be queued: again */
    }
    if (f->state == ZW_FWUPD_IDLE || now < f->deadline) {
        return;
    }
    if ((f->state == ZW_FWUPD_MD || f->state == ZW_FWUPD_REQUEST) && f->tries < ZW_FWUPD_TRIES &&
        !f->waiting_wake) {
        f->tries++;
        f->deadline = now + ZW_FWUPD_MD_MS;
        (void)send_state(f);
        return;
    }
    end(f, ZW_FWUPD_TIMEOUT, (uint8_t)f->state);
}

uint64_t zw_fwupd_next_deadline(const zw_fwupd_t *f)
{
    return f->state == ZW_FWUPD_IDLE ? UINT64_MAX : f->deadline;
}

void zw_fwupd_abort(zw_fwupd_t *f, uint64_t now)
{
    f->now = now;
    if (f->state != ZW_FWUPD_IDLE) {
        end(f, ZW_FWUPD_ABORTED, 0u);
    }
}

bool zw_fwupd_running(const zw_fwupd_t *f)
{
    return f->state != ZW_FWUPD_IDLE;
}
