/*
 * zb_bringup.h - portable radio bring-up policy (docs/features/20 ZB-001).
 *
 * This is a tested state machine, NOT a working hardware updater. The platform must supply
 * a verified compatible probe, bootloader transport, image writer and restart implementation.
 * The Elecrow socket exposes UART1 with no host BOOT/RESET lines; ROM access over that wire
 * is unverified. Blank flash does not establish download mode. See docs/features/20/README.md.
 *
 * The caller owns state and serialization. It must prevent arming throughout an actual update;
 * may_update is checked before each destructive step as a second guard. With permission revoked
 * a partial operation pauses in place. Resume only when safe. A transfer is successful only
 * after restart and a matching version probe. Repeated failures have bounded retries.
 *
 * Portable C11, no heap or RTOS. Tests use a fake radio already in a suitable download mode;
 * they do not establish first-install or same-wire flashing support for physical hardware.
 */
#ifndef ZB_BRINGUP_H
#define ZB_BRINGUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZB_BOOT_VERSION_LEN 64

#define ZB_BOOT_PROBE_MS     3000u /* how long one "who are you?" may take */
#define ZB_BOOT_PROBE_TRIES  3u    /* ... and how often we ask before trying the bootloader */
#define ZB_BOOT_SYNC_MS      500u  /* one knock on the ROM bootloader */
#define ZB_BOOT_SYNC_TRIES   8u    /* a module that has just been told to reboot needs a moment */
#define ZB_BOOT_FLASH_TRIES  2u    /* whole-image attempts before giving up on this route */
#define ZB_BOOT_RESTART_MS   2000u /* let the radio boot before asking it anything */
#define ZB_BOOT_RETRY_MS     60000u /* how long before a failed bring-up is tried again */

typedef enum {
    ZB_BOOT_IDLE = 0,      /* not started */
    ZB_BOOT_PROBE,         /* asking the radio which firmware it runs */
    ZB_BOOT_WAIT_IDLE,     /* an update is due, but a partition is armed */
    ZB_BOOT_TO_BOOTLOADER, /* asking a running radio to reboot into its ROM bootloader */
    ZB_BOOT_SYNC,          /* knocking on that bootloader */
    ZB_BOOT_FLASH,         /* writing the image over the UART */
    ZB_BOOT_RESTART,       /* out of the bootloader, into the new image */
    ZB_BOOT_RUNNING,       /* the radio runs the image this firmware carries */
    ZB_BOOT_NEEDS_HELP,    /* setup or recovery is required */
    ZB_BOOT_NO_RADIO,      /* no protocol/loader response; presence is unknown */
} zb_boot_state_t;

/* What one probe found. */
typedef enum {
    ZB_PROBE_WAIT = 0, /* no answer yet; ask again */
    ZB_PROBE_OK,       /* a version string was read */
    ZB_PROBE_SILENT,   /* the radio is not speaking this protocol */
} zb_probe_t;

typedef struct {
    void *ctx;
    /* Ask the radio which firmware it runs. Fills version when it answers. */
    zb_probe_t (*probe)(void *ctx, char *version, size_t len);
    /* Ask a running radio to reboot into its ROM bootloader (the Spinel reset-to-bootloader
     * capability). false when it cannot be asked - which is the case we cannot recover from. */
    bool (*to_bootloader)(void *ctx);
    /* Knock on the ROM bootloader (the loader's SYNC). A module already in download mode is
     * eligible only if the loader uses the connected UART. */
    bool (*sync)(void *ctx);
    /* Write the next part of the image. Returns the percentage written (0..100), or a negative
     * number when the write failed. Called repeatedly so nothing blocks the panel for minutes. */
    int (*flash_step)(void *ctx);
    /* Give up on the current write and start the next attempt from the beginning. */
    void (*flash_reset)(void *ctx);
    /* Leave the bootloader and run the image. */
    bool (*restart)(void *ctx);
} zb_boot_port_t;

typedef struct {
    zb_boot_port_t port;
    char want[ZB_BOOT_VERSION_LEN];  /* the version this firmware carries */
    char found[ZB_BOOT_VERSION_LEN]; /* what the radio last said, "" = nothing yet */
    uint8_t state;                   /* zb_boot_state_t */
    uint8_t tries;                   /* attempts in the current state */
    uint8_t flash_tries;
    uint8_t percent;                 /* while flashing */
    bool seen;                       /* the radio has answered at least once */
    bool updated;                    /* an installed image was verified after restart */
    uint64_t at;                     /* when the current state began */
    uint64_t next;                   /* when the machine wants its next tick */
} zb_boot_t;

/* 'want' is the version string of the image this firmware carries (the one the radio reports
 * back when it runs it). An empty 'want' means "take whatever is there", for a panel built
 * without an image to give. */
void zb_boot_init(zb_boot_t *b, const zb_boot_port_t *port, const char *want);

/* Begin, or begin again after the radio was lost. */
void zb_boot_start(zb_boot_t *b, uint64_t now);

/* Drive it. may_update is false while any partition is armed: writing the radio takes every
 * wireless sensor off the air, so it waits instead (ZB-001). */
void zb_boot_tick(zb_boot_t *b, uint64_t now, bool may_update);

/* When it wants the next tick; UINT64_MAX when it is finished and has nothing to wait for. */
uint64_t zb_boot_next(const zb_boot_t *b);

/* The radio stopped answering while it was running: start over. */
void zb_boot_lost(zb_boot_t *b, uint64_t now);

zb_boot_state_t zb_boot_state(const zb_boot_t *b);
/* The radio is up and running the firmware we want: the Zigbee stack may be started. */
bool zb_boot_ready(const zb_boot_t *b);
/* One line for the Settings page and the console, in the owner's words. */
const char *zb_boot_words(const zb_boot_t *b);

#ifdef __cplusplus
}
#endif

#endif /* ZB_BRINGUP_H */
