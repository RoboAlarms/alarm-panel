/*
 * zw_host.c - the link to the ZAC93 and what keeps it up: start-up (serial-api.md §5 steps 0, 1
 * and 19 here, 2-18 in zw_host_steps.c), the bootloader's "run" (§5 step 1, §8), recovery and the
 * retry schedule (§7), unexpected restarts (§2). See zw_host.h.
 *
 * The transport calls back from inside its own calls; those callbacks only take note, and
 * zw_host_poll() acts on the notes, so every owner callback of the host's own runs from poll.
 */
#include "zw_host_internal.h"

#include <string.h>

#include "zwave_proto/zw_gbl.h"

/* §5 step 0: the reset line held low this long, then SerialAPIStarted awaited this long. */
#define ZW_RESET_PULSE_MS       20u
#define ZW_RESET_WAIT_MS      3000u
/* §5 step 1: listening for the bootloader after the NAK. */
#define ZW_LISTEN_MS           500u
/* §8: SerialAPIStarted after the bootloader's "run". */
#define ZW_BOOT_RUN_WAIT_MS   5000u
/* §5: a failing step is retried twice. */
#define ZW_STEP_TRIES            3u
/* Bounds on one start-up attempt: leaving the bootloader, §7 recoveries, restarts heard. */
#define ZW_BOOT_ESCAPES_MAX      2u
#define ZW_RECOVERIES_MAX        1u
#define ZW_RESTARTS_MAX          3u
/* §7: while down, start again after 30 s, after 5 min once 10 attempts have failed. */
#define ZW_RETRY_FAST_MS     30000u
#define ZW_RETRY_SLOW_MS    300000u
#define ZW_RETRY_FAST_COUNT     10u

/* §8: the module's own firmware update (zw_host_otw_begin). */
#define ZW_OTW_MENU_MS        5000u  /* EnterBootloader to its menu; "upload gbl" to 'C' */
#define ZW_OTW_BLOCK_MS       1000u  /* a block's ACK */
#define ZW_OTW_ABORT_MS       1000u  /* a cancel's reason and menu */
#define ZW_OTW_FINISH_MS     10000u  /* EOT to "Serial upload complete" and the menu */
#define ZW_OTW_RUN_MS        10000u  /* "run" to SerialAPIStarted */
#define ZW_OTW_SENDS             3u  /* sends of one block */
#define ZW_OTW_UPLOADS           3u  /* uploads of the file (an error 0x2X is tried again) */

/* The host's own requests: 'ZW' and the attempt's generation. */
#define ZW_HOST_TAG_BASE 0x5A570000u
#define ZW_HOST_TAG_MASK 0xFFFF0000u
#define ZW_BYTE_NAK 0x15u

enum {
    H_IDLE = 0,
    H_DOWN,       /* waiting for the next attempt (timer) */
    H_RESET_LOW,  /* the reset line held low (timer: 20 ms) */
    H_RESET_WAIT, /* released: waiting for SerialAPIStarted (timer: 3 s) */
    H_LISTEN,     /* step 1: NAK written, listening for the bootloader (timer: 500 ms) */
    H_BOOT_RUN,   /* the bootloader's "run" chosen: waiting for SerialAPIStarted (timer: 5 s) */
    H_STEPS,      /* a start-up command queued or in flight */
    H_SOFT_RESET, /* §7: SoftReset queued or in flight */
    H_UP,
    H_OTW         /* §8: its own firmware update (h->otw) */
};

/* ---- The transport's callbacks: notes only ------------------------------------------------ */

static bool tr_write(void *ctx, const uint8_t *bytes, size_t len)
{
    zw_host_t *h = ctx;
    return h->port.write(h->port.ctx, bytes, len);
}

static void tr_frame(void *ctx, const zw_frame_t *frame)
{
    zw_host_t *h = ctx;
    if (h->port.frame != NULL) {
        h->port.frame(h->port.ctx, frame);
    }
}

/* Text is kept newest last, the oldest dropped when it does not fit: a bootloader menu that
 * comes in several reads is whole once its last piece is in, whatever came before it. */
static void tr_text(void *ctx, const uint8_t *bytes, size_t len)
{
    zw_host_t *h = ctx;
    h->stats.text_bytes += (uint32_t)len;
    h->text_new = true;
    if (len >= sizeof h->text) {
        memcpy(h->text, &bytes[len - sizeof h->text], sizeof h->text);
        h->text_len = sizeof h->text;
        return;
    }
    const size_t room = sizeof h->text - h->text_len;
    if (len > room) {
        const size_t drop = len - room;
        memmove(h->text, &h->text[drop], h->text_len - drop);
        h->text_len -= drop;
    }
    memcpy(&h->text[h->text_len], bytes, len);
    h->text_len += len;
}

static void tr_event(void *ctx, zw_transport_event_t ev, const zw_frame_t *frame)
{
    zw_host_t *h = ctx;
    if (ev == ZW_TRANSPORT_NOT_ANSWERING) {
        h->not_answering = true;
    } else if (ev == ZW_TRANSPORT_MODULE_RESTARTED && frame != NULL) {
        const size_t n = frame->payload_len < sizeof h->restart ? frame->payload_len
                                                                : sizeof h->restart;
        h->restart_heard = true;
        h->restart_len = n;
        if (n > 0u) {
            memcpy(h->restart, frame->payload, n);
        }
    }
}

/* A start-up or recovery command of the host's ended (one of an attempt given up since is
 * ignored). */
static void own_done(void *ctx, uint32_t tag, const zw_tx_result_t *r)
{
    zw_host_t *h = ctx;
    if (tag != (ZW_HOST_TAG_BASE | (unsigned)h->gen)) {
        return;
    }
    const uint8_t *p = r->callback != NULL ? r->callback : r->response;
    const size_t len = r->callback != NULL ? r->callback_len : r->response_len;
    const size_t n = len < sizeof h->result ? len : sizeof h->result;
    h->result_ready = true;
    h->result_status = r->status;
    h->result_len = n;
    if (n > 0u) {
        memcpy(h->result, p, n);
    }
}

/* ---- Moving between phases ---------------------------------------------------------------- */

static void submit_own(zw_host_t *h, zw_request_t *req)
{
    req->prio = ZW_PRIO_MODULE;
    req->done = own_done;
    req->ctx = h;
    req->tag = ZW_HOST_TAG_BASE | (unsigned)h->gen;
    if (zw_transport_submit(&h->tr, req) != ZW_SUBMIT_OK) {
        /* Cannot happen: the owner's requests leave the host a slot, and the host has one
         * command at a time. Taken as a failed command, so the sequence still moves on. */
        h->result_ready = true;
        h->result_status = ZW_TX_NO_ACK;
        h->result_len = 0u;
    }
}

static void link_down(zw_host_t *h, uint64_t now, zw_down_reason_t reason, uint8_t detail)
{
    h->link = ZW_LINK_DOWN;
    h->phase = H_DOWN;
    h->down.reason = reason;
    h->down.step = h->step;
    h->down.detail = detail;
    h->failures++;
    h->stats.link_downs++;
    h->timer = now + (h->failures <= ZW_RETRY_FAST_COUNT ? ZW_RETRY_FAST_MS : ZW_RETRY_SLOW_MS);
    zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
    /* Everything that needs the link fails now; its owners try again when it is up. */
    (void)zw_transport_fail_queued(&h->tr, ZW_PRIO_ALL & ~ZW_PRIO_BIT(ZW_PRIO_MODULE),
                                   ZW_TX_LINK_DOWN);
    if (h->port.link_down != NULL) {
        h->port.link_down(h->port.ctx, &h->down);
    }
}

/* §5 step 19: the link is up. The owner's link_up is where the home ID is compared with the
 * stored network, the node list reconciled, interviews started (Z5) and SmartStart opened (Z10). */
static void link_up(zw_host_t *h)
{
    h->link = ZW_LINK_UP;
    h->phase = H_UP;
    h->probing = false;
    h->timer = UINT64_MAX;
    h->failures = 0u;
    h->down.reason = ZW_DOWN_NONE;
    h->down.step = 0u;
    h->down.detail = 0u;
    h->text_len = 0u; /* while up, text is looked at afresh (the bootloader, below) */
    h->stats.link_ups++;
    zw_transport_set_gate(&h->tr, ZW_PRIO_BACKGROUND);
    if (h->port.link_up != NULL) {
        h->port.link_up(h->port.ctx, &h->facts);
    }
}

/* The start-up command at the cursor, or step 19. */
static void run_steps(zw_host_t *h)
{
    zw_request_t req;
    uint8_t buf[ZW_PAYLOAD_MAX];
    memset(&req, 0, sizeof req);
    if (!zw_host_step_plan(h, &req, buf)) {
        link_up(h);
        return;
    }
    if (h->step >= 12u) {
        h->resume_at_13 = true; /* steps 2-11 are behind: a restart resumes at step 13 */
    }
    h->phase = H_STEPS;
    h->timer = UINT64_MAX;
    submit_own(h, &req);
}

/* Steps 13-19 again, with this SerialAPIStarted as step 12's answer. */
static void resume_at_13(zw_host_t *h, const uint8_t *started, size_t len)
{
    zw_host_step_started(h, started, len);
    h->link = ZW_LINK_STARTING;
    h->step = 13u;
    h->sub = 0u;
    h->tries = 0u;
    run_steps(h);
}

/* §5 step 1: a NAK resets the module's receiver; then 500 ms to hear the bootloader. */
static void begin_listen(zw_host_t *h, uint64_t now)
{
    static const uint8_t nak = ZW_BYTE_NAK;
    (void)h->port.write(h->port.ctx, &nak, 1u);
    h->step = 1u;
    h->phase = H_LISTEN;
    h->timer = now + ZW_LISTEN_MS;
}

/* The reset line: low for 20 ms (then released and SerialAPIStarted awaited for 3 s). */
static void begin_reset(zw_host_t *h, uint64_t now, bool recovery)
{
    h->stats.reset_pulses++;
    h->recovering_line = recovery;
    h->port.reset_line(h->port.ctx, true);
    h->phase = H_RESET_LOW;
    h->timer = now + ZW_RESET_PULSE_MS;
}

/* A start-up attempt from step 0. */
static void begin_attempt(zw_host_t *h, uint64_t now)
{
    h->stats.attempts++;
    h->gen++;
    h->link = ZW_LINK_STARTING;
    h->boot_escapes = 0u;
    h->recoveries = 0u;
    h->restarts_seen = 0u;
    h->resume_at_13 = false;
    h->step = 0u;
    h->sub = 0u;
    h->tries = 0u;
    h->text_len = 0u;
    h->result_ready = false;
    h->not_answering = false;
    h->restart_heard = false;
    memset(&h->facts, 0, sizeof h->facts);
    zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
    if (h->port.reset_line != NULL) {
        begin_reset(h, now, false);
    } else {
        begin_listen(h, now);
    }
}

/* §7: a soft reset, then the reset line, then link down. */
static void begin_recovery(zw_host_t *h)
{
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_SOFT_RESET;
    h->link = ZW_LINK_RECOVERING;
    h->recoveries++;
    h->stats.recoveries++;
    h->phase = H_SOFT_RESET;
    h->timer = UINT64_MAX;
    h->text_len = 0u; /* listen afresh for the bootloader after it */
    zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
    submit_own(h, &req);
}

/* The module answered after a recovery: on from step 13 when steps 2-11 are behind, else
 * start again at step 1. */
static void recovered(zw_host_t *h, uint64_t now, const uint8_t *started, size_t len)
{
    if (h->resume_at_13) {
        resume_at_13(h, started, len);
    } else {
        h->link = ZW_LINK_STARTING;
        begin_listen(h, now);
    }
}

/* ---- The bootloader (§5 step 1, §8 step 1) ------------------------------------------------ */

static bool text_has(const zw_host_t *h, const char *needle)
{
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= h->text_len; i++) {
        if (memcmp(&h->text[i], needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* Where `needle` starts in the text, or text_len. */
static size_t text_find(const zw_host_t *h, const char *needle)
{
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= h->text_len; i++) {
        if (memcmp(&h->text[i], needle, n) == 0) {
            return i;
        }
    }
    return h->text_len;
}

/* The digit of the menu line "N. <name>" - lines `N. option` after the banner - or 0. */
static uint8_t menu_option(const zw_host_t *h, const char *name)
{
    const size_t nl = strlen(name);
    size_t i = 0u;
    while (i < h->text_len) {
        size_t start = i;
        while (i < h->text_len && h->text[i] != '\n') {
            i++;
        }
        size_t end = i; /* the line is text[start..end) */
        i++;
        while (start < end && (h->text[start] == ' ' || h->text[start] == '\r')) {
            start++;
        }
        while (end > start && (h->text[end - 1u] == ' ' || h->text[end - 1u] == '\r')) {
            end--;
        }
        if (end - start == 3u + nl && h->text[start] >= '0' && h->text[start] <= '9' &&
            h->text[start + 1u] == '.' && h->text[start + 2u] == ' ' &&
            memcmp(&h->text[start + 3u], name, nl) == 0) {
            return h->text[start];
        }
    }
    return 0u;
}

static uint8_t run_option(const zw_host_t *h)
{
    return menu_option(h, "run");
}

/* The menu's "run" (§8 step 5): its digit written, SerialAPIStarted awaited 5 s. */
static void choose_run(zw_host_t *h, uint64_t now, uint8_t digit)
{
    h->boot_escapes++;
    h->stats.bootloader++;
    h->text_len = 0u;
    (void)h->port.write(h->port.ctx, &digit, 1u);
    h->phase = H_BOOT_RUN;
    h->timer = now + ZW_BOOT_RUN_WAIT_MS;
}

/* The listening window is over: the bootloader's menu, or on to step 2. */
static void listened(zw_host_t *h, uint64_t now)
{
    if (!text_has(h, "Gecko Bootloader")) {
        h->step = 2u;
        h->sub = 0u;
        h->tries = 0u;
        run_steps(h);
        return;
    }
    const uint8_t digit = run_option(h);
    if (digit == 0u || h->boot_escapes >= ZW_BOOT_ESCAPES_MAX) {
        link_down(h, now, ZW_DOWN_BOOTLOADER, 0u);
        return;
    }
    choose_run(h, now, digit);
}

/* The module fell into its bootloader while the link was up: its whole menu - the banner and a
 * "N. run" line - came unasked. At once and by name, not seconds later as "not answering": the
 * command in flight ends (nothing more of it may reach the bootloader, where every byte is a
 * key), the gate closes, the owner hears a restart, and "run" goes now; its SerialAPIStarted
 * brings the start-up again from step 1, as after the bootloader at start (the same bounds: a
 * second fall there takes the link down, "bootloader"). */
static void fell_into_bootloader(zw_host_t *h, uint64_t now)
{
    const uint8_t digit = run_option(h);
    h->stats.bootloader_falls++;
    h->link = ZW_LINK_RECOVERING;
    h->step = 1u;
    h->boot_escapes = 0u;
    h->recoveries = 0u;
    h->restarts_seen = 0u;
    h->resume_at_13 = false;
    zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
    zw_transport_halt(&h->tr, ZW_TX_MODULE_RESTARTED);
    if (h->port.restarted != NULL) {
        h->port.restarted(h->port.ctx, ZW_HOST_RESTART_BOOTLOADER);
    }
    choose_run(h, now, digit);
}

/* ---- Acting on the notes ------------------------------------------------------------------ */

static void on_restart(zw_host_t *h, uint64_t now)
{
    switch (h->phase) {
    case H_UP:
        /* §2: restarted by itself. Steps 13-19 again behind a closed gate (§7). */
        h->stats.restarts++;
        h->restarts_seen = 0u;
        h->recoveries = 0u;
        zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
        if (h->port.restarted != NULL) {
            h->port.restarted(h->port.ctx, h->restart_len > 0u ? (unsigned)h->restart[0] : 0xFFu);
        }
        resume_at_13(h, h->restart, h->restart_len);
        break;
    case H_STEPS:
        h->result_ready = false; /* the command in flight ended with it */
        if (++h->restarts_seen > ZW_RESTARTS_MAX) {
            link_down(h, now, ZW_DOWN_RESTARTING, 0u);
        } else if (h->step >= 13u) {
            resume_at_13(h, h->restart, h->restart_len);
        } else {
            run_steps(h); /* the same command again: the restart was not its fault */
        }
        break;
    case H_RESET_WAIT:
        if (h->recovering_line) {
            recovered(h, now, h->restart, h->restart_len);
        } else {
            begin_listen(h, now); /* step 0 done */
        }
        break;
    case H_BOOT_RUN:
        begin_listen(h, now); /* out of the bootloader: start again at step 1 */
        break;
    case H_DOWN:
        begin_attempt(h, now); /* back by itself (plugged in again): no need to wait */
        break;
    default:
        break; /* while listening or holding the line: nothing to do */
    }
}

static void on_result(zw_host_t *h, uint64_t now)
{
    if (h->phase == H_SOFT_RESET) {
        if (h->result_status == ZW_TX_OK) {
            recovered(h, now, h->result, h->result_len);
        } else if (h->port.reset_line != NULL) {
            begin_reset(h, now, true);
        } else {
            link_down(h, now, ZW_DOWN_NOT_ANSWERING, 0u);
        }
        return;
    }
    if (h->phase == H_UP && h->probing) {
        /* the idle probe (ZW_HOST_PROBE_IDLE_MS): a failed one already told the transport's
         * NOT_ANSWERING; another home ID is another module */
        h->probing = false;
        if (h->result_status == ZW_TX_OK && h->result_len >= 4u &&
            ((uint32_t)h->result[0] << 24 | (uint32_t)h->result[1] << 16 |
             (uint32_t)h->result[2] << 8 | (uint32_t)h->result[3]) != h->facts.home_id) {
            h->recoveries = 0u;
            h->step = 0u;
            begin_recovery(h);
        } else if (h->result_status != ZW_TX_OK && h->phase == H_UP) {
            h->recoveries = 0u;
            h->step = 0u;
            begin_recovery(h); /* §7, whatever the transport said */
        }
        return;
    }
    if (h->phase != H_STEPS) {
        return;
    }
    zw_link_down_t refusal = {ZW_DOWN_NONE, 0u, 0u};
    bool bad_answer = false;
    const zw_step_verdict_t v = zw_host_step_result(h, h->result_status, h->result,
                                                    h->result_len, &refusal, &bad_answer);
    if (v == ZW_STEP_NEXT) {
        h->tries = 0u;
        run_steps(h);
    } else if (v == ZW_STEP_REFUSE) {
        link_down(h, now, refusal.reason, refusal.detail);
    } else if (++h->tries < ZW_STEP_TRIES) {
        h->stats.step_retries++;
        run_steps(h);
    } else if (h->recoveries < ZW_RECOVERIES_MAX) {
        begin_recovery(h); /* §5: a step that still fails is §7's */
    } else {
        link_down(h, now, bad_answer ? ZW_DOWN_BAD_ANSWER : ZW_DOWN_NOT_ANSWERING, 0u);
    }
}

static void on_timer(zw_host_t *h, uint64_t now)
{
    switch (h->phase) {
    case H_RESET_LOW:
        h->port.reset_line(h->port.ctx, false);
        h->phase = H_RESET_WAIT;
        h->timer = now + ZW_RESET_WAIT_MS;
        break;
    case H_RESET_WAIT:
        if (h->recovering_line) {
            link_down(h, now, ZW_DOWN_NOT_ANSWERING, 0u); /* §7: still silent */
        } else {
            begin_listen(h, now); /* no SerialAPIStarted: step 1 all the same */
        }
        break;
    case H_LISTEN:
        listened(h, now);
        break;
    case H_BOOT_RUN:
        link_down(h, now, ZW_DOWN_BOOTLOADER, 0u);
        break;
    case H_DOWN:
        begin_attempt(h, now);
        break;
    default:
        break;
    }
}

/* ---- The module's own firmware (§8) ------------------------------------------------------- */

static void otw_write(zw_host_t *h, const uint8_t *bytes, size_t len)
{
    (void)h->port.write(h->port.ctx, bytes, len);
}

/* The bootloader's menu is whole: its banner, "upload gbl" and "run". */
static bool otw_menu(const zw_host_t *h)
{
    return text_has(h, "Gecko Bootloader") && menu_option(h, "upload gbl") != 0u &&
           run_option(h) != 0u;
}

/* The update ended: the owner hears it, then the start-up from step 0 - after a failure, its
 * own bootloader handling chooses "run" and finds whether the old firmware is still there. */
static void otw_end(zw_host_t *h, uint64_t now, zw_otw_result_t result, uint8_t error)
{
    h->otw.p.state = ZW_OTW_DONE;
    h->otw.p.result = (uint8_t)result;
    h->otw.p.error = error;
    h->otw.image = NULL;
    h->otw.len = 0u;
    h->phase = H_IDLE;
    h->timer = UINT64_MAX;
    if (h->port.otw_done != NULL) {
        h->port.otw_done(h->port.ctx);
    }
    begin_attempt(h, now);
}

/* §8 step 2: the menu's "upload gbl"; "begin upload", then 'C', awaited. */
static void otw_choose_upload(zw_host_t *h, uint64_t now)
{
    const uint8_t key = menu_option(h, "upload gbl");
    h->otw.p.attempts++;
    h->otw.p.sent = 0u;
    h->otw.tries = 0u;
    h->otw.ready = false;
    h->text_len = 0u;
    otw_write(h, &key, 1u);
    h->otw.p.state = ZW_OTW_STARTING;
    h->timer = now + ZW_OTW_MENU_MS;
}

/* §8 step 3: the block not yet acknowledged, sent (again). */
static void otw_send_block(zw_host_t *h, uint64_t now)
{
    uint8_t frame[ZW_XMODEM_FRAME];
    if (!zw_xmodem_frame(h->otw.image, h->otw.len, h->otw.p.sent, frame)) {
        return;
    }
    if (h->otw.tries > 0u) {
        h->otw.p.resent++;
    }
    h->otw.tries++;
    otw_write(h, frame, sizeof frame);
    h->timer = now + ZW_OTW_BLOCK_MS;
}

/* The block again, or - its third send refused - the upload given up. */
static void otw_again(zw_host_t *h, uint64_t now)
{
    if (h->otw.tries >= ZW_OTW_SENDS) {
        static const uint8_t cancel[] = {ZW_XMODEM_CAN, ZW_XMODEM_CAN, ZW_XMODEM_CAN};
        otw_write(h, cancel, sizeof cancel);
        otw_end(h, now, ZW_OTW_RETRIES, 0u);
        return;
    }
    otw_send_block(h, now);
}

/* The bootloader's error code in its text ("error 0x18"), 0 when none. */
static uint8_t otw_error_code(const zw_host_t *h)
{
    size_t at = text_find(h, "0x");
    if (at == h->text_len) {
        return 0u;
    }
    unsigned v = 0u;
    for (at += 2u; at < h->text_len; at++) {
        const uint8_t c = h->text[at];
        const unsigned d = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                           : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                           : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                                  : 16u;
        if (d > 15u || v > 0x0Fu) {
            break;
        }
        v = v << 4 | d;
    }
    return (uint8_t)v;
}

/* A cancel's reason is in (or its time is up): an error 0x2X is tried again from the menu. */
static void otw_aborted(zw_host_t *h, uint64_t now)
{
    const uint8_t error = otw_error_code(h);
    if (((unsigned)error & 0xF0u) == 0x20u && h->otw.p.attempts < ZW_OTW_UPLOADS && otw_menu(h)) {
        otw_choose_upload(h, now);
        return;
    }
    otw_end(h, now, ZW_OTW_ABORTED, error);
}

static void otw_poll(zw_host_t *h, uint64_t now)
{
    zw_otw_progress_t *p = &h->otw.p;
    const bool control = h->otw.control_new;
    const uint8_t byte = h->otw.control;
    const bool restart = h->restart_heard;
    const bool timed_out = h->timer <= now;
    h->otw.control_new = false;
    h->restart_heard = false;
    h->text_new = false;
    if (timed_out) {
        h->timer = UINT64_MAX;
    }
    switch (p->state) {
    case ZW_OTW_ENTERING:
        if (otw_menu(h)) {
            otw_choose_upload(h, now);
        } else if (timed_out) {
            otw_end(h, now, ZW_OTW_NOT_SUPPORTED, 0u);
        }
        break;
    case ZW_OTW_STARTING: {
        const size_t begin = text_find(h, "begin upload");
        if (begin < h->text_len &&
            memchr(&h->text[begin], ZW_XMODEM_READY, h->text_len - begin) != NULL) {
            p->state = ZW_OTW_SENDING;
            h->text_len = 0u;
            otw_send_block(h, now);
        } else if (control && byte == ZW_XMODEM_CAN) {
            p->state = ZW_OTW_ABORTING;
            h->timer = now + ZW_OTW_ABORT_MS;
        } else if (timed_out) {
            otw_end(h, now, begin < h->text_len ? ZW_OTW_TIMEOUT : ZW_OTW_NOT_SUPPORTED, 0u);
        }
        break;
    }
    case ZW_OTW_SENDING:
        if (control && byte == ZW_XMODEM_ACK) {
            p->sent++;
            h->otw.tries = 0u;
            if (p->sent < p->total) {
                otw_send_block(h, now);
            } else {
                static const uint8_t eot = ZW_XMODEM_EOT;
                h->text_len = 0u;
                otw_write(h, &eot, 1u);
                p->state = ZW_OTW_FINISHING;
                h->timer = now + ZW_OTW_FINISH_MS;
            }
        } else if (control && byte == ZW_XMODEM_CAN) {
            /* its reason and menu follow the cancel - maybe already in (the text was cleared
             * when the blocks began) */
            p->state = ZW_OTW_ABORTING;
            h->timer = now + ZW_OTW_ABORT_MS;
        } else if ((control && byte == ZW_XMODEM_NAK) || timed_out) {
            otw_again(h, now);
        }
        break;
    case ZW_OTW_ABORTING:
        if (timed_out || otw_menu(h)) {
            otw_aborted(h, now);
        }
        break;
    case ZW_OTW_FINISHING:
        if (text_has(h, "Serial upload complete") && otw_menu(h)) {
            const uint8_t key = run_option(h);
            h->text_len = 0u;
            otw_write(h, &key, 1u);
            p->state = ZW_OTW_RUNNING;
            h->timer = now + ZW_OTW_RUN_MS;
        } else if (text_has(h, "aborted") && otw_menu(h)) {
            otw_aborted(h, now);
        } else if (timed_out) {
            otw_end(h, now, ZW_OTW_TIMEOUT, 0u);
        }
        break;
    case ZW_OTW_RUNNING:
        if (restart) {
            otw_end(h, now, ZW_OTW_OK, 0u);
        } else if (timed_out || otw_menu(h)) {
            otw_end(h, now, ZW_OTW_NOT_STARTED, 0u); /* back in its bootloader, or silent */
        }
        break;
    default:
        break;
    }
}

zw_otw_begin_t zw_host_otw_begin(zw_host_t *h, uint64_t now_ms, const uint8_t *image, size_t len)
{
    const bool up = h->phase == H_UP;
    const bool in_bootloader = h->phase == H_DOWN && h->down.reason == ZW_DOWN_BOOTLOADER &&
                               otw_menu(h);
    if (h->phase == H_OTW || (!up && h->phase != H_DOWN)) {
        return ZW_OTW_BUSY;
    }
    if (!up && !in_bootloader) {
        return ZW_OTW_NO_MODULE;
    }
    if (zw_gbl_check(image, len, NULL) != ZW_GBL_OK) {
        return ZW_OTW_BAD_FILE;
    }
    memset(&h->otw, 0, sizeof h->otw);
    h->otw.image = image;
    h->otw.len = len;
    h->otw.p.total = zw_xmodem_blocks(len);
    h->stats.otw_updates++;
    h->phase = H_OTW;
    h->link = ZW_LINK_DOWN;
    h->probing = false;
    h->down.reason = ZW_DOWN_UPDATING;
    h->down.step = 0u;
    h->down.detail = 0u;
    zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
    if (up) {
        zw_transport_halt(&h->tr, ZW_TX_LINK_DOWN);
    }
    (void)zw_transport_fail_queued(&h->tr, ZW_PRIO_ALL & ~ZW_PRIO_BIT(ZW_PRIO_MODULE),
                                   ZW_TX_LINK_DOWN);
    if (h->port.link_down != NULL) {
        h->port.link_down(h->port.ctx, &h->down);
    }
    if (in_bootloader) {
        otw_choose_upload(h, now_ms); /* it waits at its prompt already */
        return ZW_OTW_BEGUN;
    }
    uint8_t frame[8];
    const size_t n = zw_frame_build(frame, sizeof frame, ZW_TYPE_REQUEST,
                                    (uint8_t)ZW_FUNC_ENTER_BOOTLOADER, NULL, 0u);
    h->text_len = 0u;
    otw_write(h, frame, n);
    h->otw.p.state = ZW_OTW_ENTERING;
    h->timer = now_ms + ZW_OTW_MENU_MS;
    return ZW_OTW_BEGUN;
}

const zw_otw_progress_t *zw_host_otw(const zw_host_t *h)
{
    return &h->otw.p;
}

/* ---- API ---------------------------------------------------------------------------------- */

void zw_host_init(zw_host_t *h, const zw_host_port_t *port, const zw_host_config_t *cfg)
{
    memset(h, 0, sizeof *h);
    h->port = *port;
    if (cfg != NULL) {
        h->cfg = *cfg;
    }
    const zw_transport_port_t tp = {h, tr_write, tr_frame, tr_text, tr_event};
    zw_transport_init(&h->tr, &tp);
    zw_transport_set_gate(&h->tr, ZW_PRIO_MODULE);
    h->link = ZW_LINK_OFF;
    h->phase = H_IDLE;
    h->timer = UINT64_MAX;
}

void zw_host_start(zw_host_t *h, uint64_t now_ms)
{
    begin_attempt(h, now_ms);
}

void zw_host_set_config(zw_host_t *h, const zw_host_config_t *cfg)
{
    h->cfg = *cfg;
}

void zw_host_on_control(zw_host_t *h, uint64_t now_ms, uint8_t byte)
{
    h->heard_ms = now_ms;
    if (h->phase == H_OTW) { /* the bootloader's XMODEM answers, not the Serial API's */
        h->otw.control = byte;
        h->otw.control_new = true;
        return;
    }
    zw_transport_on_control(&h->tr, now_ms, byte);
}

void zw_host_on_frame(zw_host_t *h, uint64_t now_ms, const zw_frame_t *frame)
{
    h->heard_ms = now_ms;
    zw_transport_on_frame(&h->tr, now_ms, frame);
}

/* The idle probe is due: the link up, nothing out, nothing heard for ZW_HOST_PROBE_IDLE_MS. */
static uint64_t probe_due(const zw_host_t *h)
{
    if (h->phase != H_UP || h->probing) {
        return UINT64_MAX;
    }
    return h->heard_ms + ZW_HOST_PROBE_IDLE_MS;
}

static void probe(zw_host_t *h, uint64_t now_ms)
{
    if (probe_due(h) > now_ms || zw_transport_busy(&h->tr) || zw_transport_queued(&h->tr) != 0u) {
        return;
    }
    zw_request_t req;
    memset(&req, 0, sizeof req);
    req.func = (uint8_t)ZW_FUNC_GET_CONTROLLER_ID;
    req.expect = ZW_EXPECT_RESPONSE;
    h->probing = true;
    h->heard_ms = now_ms; /* one probe per quiet minute, whatever it gets */
    h->stats.probes++;
    submit_own(h, &req);
}

void zw_host_on_text(zw_host_t *h, uint64_t now_ms, const uint8_t *bytes, size_t len)
{
    if (h->phase == H_OTW) {
        tr_text(h, bytes, len);
        return;
    }
    zw_transport_on_text(&h->tr, now_ms, bytes, len);
}

void zw_host_poll(zw_host_t *h, uint64_t now_ms)
{
    /* A few rounds: acting may queue a command whose failure the transport reports at once (a
     * write that did not leave), which is acted on in the next round. */
    for (unsigned round = 0; round < 8u; round++) {
        if (h->phase == H_OTW) {
            otw_poll(h, now_ms);
            zw_transport_poll(&h->tr, now_ms);
            if (h->phase == H_OTW) {
                break;
            }
            continue; /* it ended: the start-up has begun */
        }
        if (h->text_new) {
            h->text_new = false;
            if (h->phase == H_UP && text_has(h, "Gecko Bootloader") && run_option(h) != 0u) {
                fell_into_bootloader(h, now_ms);
            }
        }
        if (h->restart_heard) {
            h->restart_heard = false;
            on_restart(h, now_ms);
        }
        if (h->result_ready) {
            h->result_ready = false;
            on_result(h, now_ms);
        }
        if (h->not_answering) {
            h->not_answering = false;
            if (h->phase == H_UP) {
                h->recoveries = 0u;
                h->step = 0u; /* not a start-up step */
                begin_recovery(h); /* §7; while starting, the steps' own tries decide */
            }
        }
        if (h->timer <= now_ms) {
            h->timer = UINT64_MAX;
            on_timer(h, now_ms);
        }
        probe(h, now_ms);
        zw_transport_poll(&h->tr, now_ms);
        if (!h->restart_heard && !h->result_ready && !h->not_answering && !h->text_new &&
            h->timer > now_ms) {
            break;
        }
    }
}

uint64_t zw_host_next_deadline(const zw_host_t *h)
{
    if (h->restart_heard || h->result_ready || h->not_answering || h->text_new ||
        h->otw.control_new) {
        return 0u;
    }
    if (h->phase == H_OTW) {
        return h->timer;
    }
    uint64_t t = zw_transport_next_deadline(&h->tr);
    const uint64_t p = probe_due(h);
    t = p < t ? p : t;
    return t < h->timer ? t : h->timer;
}

zw_submit_t zw_host_submit(zw_host_t *h, const zw_request_t *req)
{
    if (req == NULL || req->prio == ZW_PRIO_MODULE ||
        (req->tag & ZW_HOST_TAG_MASK) == ZW_HOST_TAG_BASE) {
        return ZW_SUBMIT_INVALID; /* never the owner's, whatever the link */
    }
    if (h->link == ZW_LINK_OFF || h->link == ZW_LINK_DOWN) {
        return ZW_SUBMIT_LINK_DOWN;
    }
    if (zw_transport_queued(&h->tr) + 1u >= ZW_TRANSPORT_QUEUE_LEN) {
        return ZW_SUBMIT_FULL; /* the last slot is the host's */
    }
    return zw_transport_submit(&h->tr, req);
}

size_t zw_host_cancel(zw_host_t *h, uint32_t tag)
{
    return (tag & ZW_HOST_TAG_MASK) == ZW_HOST_TAG_BASE ? 0u : zw_transport_cancel(&h->tr, tag);
}

bool zw_host_recover(zw_host_t *h, uint64_t now_ms)
{
    (void)now_ms;
    if (h->phase != H_UP || h->probing) {
        return false; /* starting, recovering, updating or down; or its probe's answer is due */
    }
    h->recoveries = 0u;
    h->step = 0u; /* not a start-up step */
    begin_recovery(h);
    return true;
}

zw_link_state_t zw_host_link(const zw_host_t *h)
{
    return h->link;
}

const zw_module_facts_t *zw_host_facts(const zw_host_t *h)
{
    return &h->facts;
}

const zw_link_down_t *zw_host_down_reason(const zw_host_t *h)
{
    return &h->down;
}

const zw_host_stats_t *zw_host_stats(const zw_host_t *h)
{
    return &h->stats;
}

const zw_transport_stats_t *zw_host_transport_stats(const zw_host_t *h)
{
    return &h->tr.stats;
}
