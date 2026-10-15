/*
 * bringup.c - getting the sensor radio running by itself (see zb_bringup.h).
 */
#include <stdio.h>
#include <string.h>

#include "zigbee_host/zb_bringup.h"

static uint64_t add_ms(uint64_t t, uint64_t ms)
{
    return (t > UINT64_MAX - ms) ? UINT64_MAX : t + ms;
}

static void go(zb_boot_t *b, zb_boot_state_t st, uint64_t now, uint64_t in_ms)
{
    b->state = (uint8_t)st;
    b->tries = 0u;
    b->at = now;
    b->next = add_ms(now, in_ms);
}

static void again(zb_boot_t *b, uint64_t now, uint64_t in_ms)
{
    if (b->tries < 0xFFu) {
        b->tries++;
    }
    b->at = now;
    b->next = add_ms(now, in_ms);
}

void zb_boot_init(zb_boot_t *b, const zb_boot_port_t *port, const char *want)
{
    memset(b, 0, sizeof(*b));
    if (port != NULL) {
        b->port = *port;
    }
    snprintf(b->want, sizeof(b->want), "%s", want != NULL ? want : "");
    b->state = (uint8_t)ZB_BOOT_IDLE;
    b->next = UINT64_MAX;
}

void zb_boot_start(zb_boot_t *b, uint64_t now)
{
    b->found[0] = '\0';
    b->seen = false; /* what answered last time proves nothing about what is in there now */
    b->percent = 0u;
    b->flash_tries = 0u;
    go(b, ZB_BOOT_PROBE, now, 0u);
}

void zb_boot_lost(zb_boot_t *b, uint64_t now)
{
    if (b->state == (uint8_t)ZB_BOOT_RUNNING) {
        zb_boot_start(b, now);
    }
}

zb_boot_state_t zb_boot_state(const zb_boot_t *b)
{
    return (zb_boot_state_t)b->state;
}

bool zb_boot_ready(const zb_boot_t *b)
{
    return b->state == (uint8_t)ZB_BOOT_RUNNING;
}

/* The image we carry is the one the radio should be running. An empty 'want' means this build
 * has no image to give, so whatever answers is accepted. */
static bool version_matches(const zb_boot_t *b)
{
    return b->want[0] == '\0' || strcmp(b->want, b->found) == 0;
}

/* Everything that needs the radio in its bootloader starts here: ask a running radio to reboot
 * into it, or probe a bootloader that might already be listening. Silence does not establish
 * download mode or prove that the socket is empty. The hardware port must verify its transport. */
static void head_for_the_bootloader(zb_boot_t *b, uint64_t now)
{
    if (b->want[0] == '\0' || b->port.flash_step == NULL ||
        b->flash_tries >= ZB_BOOT_FLASH_TRIES) {
        go(b, ZB_BOOT_NEEDS_HELP, now, ZB_BOOT_RETRY_MS);
        return;
    }
    if (b->seen) {
        go(b, ZB_BOOT_TO_BOOTLOADER, now, 0u);
    } else {
        go(b, ZB_BOOT_SYNC, now, 0u);
    }
}

static void tick_probe(zb_boot_t *b, uint64_t now, bool may_update)
{
    char version[ZB_BOOT_VERSION_LEN];
    version[0] = '\0';
    const zb_probe_t r = b->port.probe != NULL
                             ? b->port.probe(b->port.ctx, version, sizeof(version))
                             : ZB_PROBE_SILENT;
    if (r == ZB_PROBE_OK) {
        b->seen = true;
        snprintf(b->found, sizeof(b->found), "%s", version);
        if (version_matches(b)) {
            b->updated = b->flash_tries != 0u;
            go(b, ZB_BOOT_RUNNING, now, 0u);
            b->next = UINT64_MAX;
            return;
        }
        if (!may_update) {
            /* Armed: the sensors we already have are worth more than a newer radio image. */
            go(b, ZB_BOOT_WAIT_IDLE, now, ZB_BOOT_RETRY_MS);
            return;
        }
        head_for_the_bootloader(b, now);
        return;
    }
    if (r == ZB_PROBE_WAIT && b->tries + 1u < ZB_BOOT_PROBE_TRIES) {
        again(b, now, ZB_BOOT_PROBE_MS);
        return;
    }
    if (b->tries + 1u < ZB_BOOT_PROBE_TRIES) {
        again(b, now, ZB_BOOT_PROBE_MS);
        return;
    }
    /* No protocol response. Probe the loader if an image and writer are available;
     * factory provisioning and ROM UART access are separate hardware prerequisites. */
    if (!may_update) {
        go(b, ZB_BOOT_WAIT_IDLE, now, ZB_BOOT_RETRY_MS);
        return;
    }
    head_for_the_bootloader(b, now);
}

static void tick_to_bootloader(zb_boot_t *b, uint64_t now)
{
    const bool asked = b->port.to_bootloader != NULL && b->port.to_bootloader(b->port.ctx);
    if (!asked) {
        /* It runs something that has never heard of us. Nobody can fix that over this wire. */
        go(b, ZB_BOOT_NEEDS_HELP, now, ZB_BOOT_RETRY_MS);
        return;
    }
    go(b, ZB_BOOT_SYNC, now, 0u);
}

static void tick_sync(zb_boot_t *b, uint64_t now)
{
    if (b->port.sync != NULL && b->port.sync(b->port.ctx)) {
        if (b->flash_tries >= ZB_BOOT_FLASH_TRIES) {
            go(b, ZB_BOOT_NEEDS_HELP, now, ZB_BOOT_RETRY_MS);
            return;
        }
        b->flash_tries++;
        b->seen = true; /* the ROM bootloader answered: there is a module in the socket */
        if (b->port.flash_reset != NULL) {
            b->port.flash_reset(b->port.ctx);
        }
        b->percent = 0u;
        go(b, ZB_BOOT_FLASH, now, 0u);
        return;
    }
    if (b->tries + 1u < ZB_BOOT_SYNC_TRIES) {
        again(b, now, ZB_BOOT_SYNC_MS);
        return;
    }
    /* Distinguish a previously identified module from an inconclusive lack of response. */
    go(b, b->seen ? ZB_BOOT_NEEDS_HELP : ZB_BOOT_NO_RADIO, now, ZB_BOOT_RETRY_MS);
}

static void tick_flash(zb_boot_t *b, uint64_t now)
{
    const int p = b->port.flash_step != NULL ? b->port.flash_step(b->port.ctx) : -1;
    if (p < 0) {
        if (b->flash_tries < ZB_BOOT_FLASH_TRIES) {
            if (b->port.flash_reset != NULL) {
                b->port.flash_reset(b->port.ctx);
            }
            b->percent = 0u;
            go(b, ZB_BOOT_SYNC, now, ZB_BOOT_SYNC_MS);
            return;
        }
        go(b, b->seen ? ZB_BOOT_NEEDS_HELP : ZB_BOOT_NO_RADIO, now, ZB_BOOT_RETRY_MS);
        return;
    }
    b->percent = p > 100 ? 100u : (uint8_t)p;
    if (b->percent >= 100u) {
        go(b, ZB_BOOT_RESTART, now, 0u);
        return;
    }
    b->next = now; /* keep writing; the caller decides how often it comes back */
}

static void tick_restart(zb_boot_t *b, uint64_t now)
{
    if (b->port.restart == NULL || !b->port.restart(b->port.ctx)) {
        go(b, ZB_BOOT_NEEDS_HELP, now, ZB_BOOT_RETRY_MS);
        return;
    }
    b->found[0] = '\0';
    go(b, ZB_BOOT_PROBE, now, ZB_BOOT_RESTART_MS);
}

void zb_boot_tick(zb_boot_t *b, uint64_t now, bool may_update)
{
    if (b->state == (uint8_t)ZB_BOOT_IDLE || b->state == (uint8_t)ZB_BOOT_RUNNING) {
        return;
    }
    if (now < b->next) {
        return;
    }
    /* Recheck at every destructive step: the panel can arm between ticks. Keep the
     * current phase so an interrupted write resumes only after the caller allows it. */
    if (!may_update && (b->state == ZB_BOOT_TO_BOOTLOADER || b->state == ZB_BOOT_SYNC ||
                       b->state == ZB_BOOT_FLASH || b->state == ZB_BOOT_RESTART)) {
        b->next = add_ms(now, ZB_BOOT_RETRY_MS);
        return;
    }
    switch ((zb_boot_state_t)b->state) {
    case ZB_BOOT_PROBE:
        tick_probe(b, now, may_update);
        break;
    case ZB_BOOT_WAIT_IDLE:
        /* Look again when the panel is disarmed; until then, say why nothing is happening. */
        if (may_update) {
            go(b, ZB_BOOT_PROBE, now, 0u);
        } else {
            b->next = add_ms(now, ZB_BOOT_RETRY_MS);
        }
        break;
    case ZB_BOOT_TO_BOOTLOADER:
        tick_to_bootloader(b, now);
        break;
    case ZB_BOOT_SYNC:
        tick_sync(b, now);
        break;
    case ZB_BOOT_FLASH:
        tick_flash(b, now);
        break;
    case ZB_BOOT_RESTART:
        tick_restart(b, now);
        break;
    case ZB_BOOT_NEEDS_HELP:
    case ZB_BOOT_NO_RADIO:
        /* Somebody may have pressed the buttons, or plugged a module in, since we last looked. */
        zb_boot_start(b, now);
        break;
    case ZB_BOOT_IDLE:
    case ZB_BOOT_RUNNING:
    default:
        break;
    }
}

uint64_t zb_boot_next(const zb_boot_t *b)
{
    return b->next;
}

const char *zb_boot_words(const zb_boot_t *b)
{
    switch ((zb_boot_state_t)b->state) {
    case ZB_BOOT_IDLE:
        return "Sensor radio off";
    case ZB_BOOT_PROBE:
        return "Looking for the sensor radio";
    case ZB_BOOT_WAIT_IDLE:
        return "Sensor radio update waits until the system is disarmed";
    case ZB_BOOT_TO_BOOTLOADER:
    case ZB_BOOT_SYNC:
        return "Preparing the sensor radio";
    case ZB_BOOT_FLASH:
        return "Updating the sensor radio";
    case ZB_BOOT_RESTART:
        return "Restarting the sensor radio";
    case ZB_BOOT_RUNNING:
        return "Sensor radio ready";
    case ZB_BOOT_NEEDS_HELP:
        return "Sensor radio setup required; see the installation guide";
    case ZB_BOOT_NO_RADIO:
    default:
        return "Sensor radio not responding";
    }
}
