/*
 * plink_fw.h - the master's keypad-update planner (docs/features/11 "Updates through the link":
 * The rules, The master; LINK-030, LINK-031).
 *
 * plink_fw_plan is a small pure function, run from the master service's poll. Keypads follow their
 * master: every keypad behind the target is brought to it with no person's step, and restarts with
 * the master. The rules, per keypad (a keypad of the target or newer is left alone: never older or
 * equal, OTA-014; nothing goes to one whose live session lacks PLINK_CAP_FW, on frames under 250 B,
 * or not heard for 30 s):
 *   - One order at a time: a new one only once the last is echoed (the core resends it).
 *   - INFO names the master's confirmed version at each session of the keypad and when it changes
 *     (its cap and its trial depend on it); nothing else goes in a session before its echo.
 *   - FETCH{V, server, channel, key} to a keypad behind V that fetches by itself.
 *   - RELAY{V} when its FETCH could not be done (not reached 1-9, BROKE_OFF 20, TOO_SLOW 21, or the
 *     server offering another version, nothing newer or no release of V 13-15), or at once to one
 *     that can't fetch: from the master's own image of V (running or spare slot) when it is the
 *     same device, within the image's chip revisions, of a project it accepts and fits its slot;
 *     else from a copy fetched with its facts (one copy at a time; the step names the one
 *     wanted).
 *     A relay under way whose image the core no longer serves (the master restarted) is sent again:
 *     the keypad goes on by id8.
 *   - INSTALL{V} to every READY keypad once a session, when the master's own restart is allowed
 *     (own_restart: the service then waits up to 5 s for the echoes, step.installing, and restarts;
 *     a keypad installing alone - plink_master_fw_busy - holds own_restart off, but keypads
 *     installing with it once its INSTALLs are out don't), and at once when the master already
 *     runs V confirmed (a keypad found behind later catches up).
 *   - A failed download is tried again after an hour (and when V changes); a refusal by the server
 *     (10-12, 16-19) or of the image (30-49 but BUDGET) waits for the master's next version.
 *   - A keypad that went back on V (its FW_STATE's last section says ROLLED_BACK V, or it came back
 *     behind V in a later session than its INSTALL without V READY) isn't ordered V again until the
 *     target changes, so it never restarts in a loop.
 *
 * Portable C11, no heap, no globals; everything is the caller's memory. Times are the master's
 * clock (now_ms).
 */
#ifndef PLINK_FW_H
#define PLINK_FW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_link/plink_crypto.h"
#include "panel_link/plink_frame.h"
#include "panel_link/plink_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLINK_FWP_RETRY_MS 3600000u /* a failed download is tried again after this */
#define PLINK_FWP_PAGE     1024u    /* a RELAY's page, count and gap (each side clamps the page */
#define PLINK_FWP_COUNT    16u      /* to its frame): 16 KB a request on UDP */
#define PLINK_FWP_GAP_MS   0u

/* A keypad as the master's link sees it (plink_master_fw_keypad fills it). */
typedef struct {
    uint8_t device;          /* its number (0: no keypad here) */
    bool fw_cap;             /* its live session holds PLINK_CAP_FW ... */
    bool fw;                 /* ... on frames of 250 B or more: orders go */
    bool online;             /* a live session, heard within the last 30 s */
    uint32_t sessions;       /* moves with each new session (a restart shows as one) */
    bool have_info;          /* a FW_INFO came since this master started ... */
    bool info_now;           /* ... in its live session */
    plink_fw_info_t info;
    bool have_state;         /* a FW_STATE came since this master started ... */
    bool state_now;          /* ... in its live session */
    plink_fw_state_t state;
    uint32_t order;          /* the newest order (0: none since this master started) ... */
    uint8_t order_op;        /* ... its op ... */
    bool order_acked;        /* ... echoed by a FW_STATE (no more resends) ... */
    uint8_t order_why;       /* ... with this why (its refusal when it was refused) */
    bool relay;              /* the core serves this keypad's FW_REQs for the image of relay_id8 */
    uint8_t relay_id8[PLINK_ID8_LEN];
    bool busy;               /* installing or on its self-test (supervision held, up to 10 min) */
} plink_fw_kp_t;

/* Where an image the master can relay is (the service's port.fw_read finds it by id8). */
typedef enum {
    PLINK_FWSRC_NONE = 0, /* no image */
    PLINK_FWSRC_RUNNING,  /* the slot it runs */
    PLINK_FWSRC_SPARE,    /* its spare slot: its own update, downloaded and checked */
    PLINK_FWSRC_COPY,     /* a copy fetched for keypads of another model (PSRAM) */
} plink_fw_src_t;

/* An image the master can relay, with its facts. */
typedef struct {
    uint8_t src;                      /* plink_fw_src_t (NONE: no image) */
    char version[PLINK_FW_VERSION_MAX + 1];
    char device[PLINK_FW_DEVICE_MAX + 1];
    char project[PLINK_FW_PROJECT_MAX + 1];
    uint16_t chip_min, chip_max;      /* the chip revisions it runs on (chip_rev_full, inclusive) */
    uint32_t size;
    uint8_t sha256[PLINK_SHA256_LEN]; /* its first 8 bytes are the relay's id8 */
} plink_fw_image_t;

/* What a copy is fetched with (panel_ota_fetch_for): the version, and a keypad's facts. */
typedef struct {
    char version[PLINK_FW_VERSION_MAX + 1];  /* "": none */
    char device[PLINK_FW_DEVICE_MAX + 1];
    char chip_rev[PLINK_FW_CHIP_MAX + 1];
    char project[PLINK_FW_PROJECT_MAX + 1];
} plink_fw_facts_t;

typedef enum {
    PLINK_FWCOPY_NONE = 0,
    PLINK_FWCOPY_FETCHING,
    PLINK_FWCOPY_READY,
    PLINK_FWCOPY_FAILED, /* why: the server's refusal, the image's, or a reach or transfer reason */
} plink_fwcopy_t;

/* The copy the service holds (one at a time). */
typedef struct {
    uint8_t state;           /* plink_fwcopy_t */
    uint8_t why;             /* FAILED: a plink_fw_why_t */
    plink_fw_facts_t facts;  /* what it was fetched with */
    plink_fw_image_t image;  /* READY: the image (src COPY) */
} plink_fw_copy_t;

/* What the planner remembers of one keypad between steps (the caller keeps it, RAM; zeros to
 * start). */
typedef struct {
    char target[PLINK_FW_VERSION_MAX + 1];    /* the target what follows is about */
    uint8_t op;              /* the image order under way for it: FETCH or RELAY (0: none) */
    uint8_t why;             /* the last failure's why (0: none) */
    bool wait_next;          /* refused: waits for the master's next version */
    uint64_t retry_at;       /* a failed download: tried again then (0: not waiting) */
    uint32_t install_sid;    /* the session its INSTALL for target went in (0: none) */
    char went_back[PLINK_FW_VERSION_MAX + 1]; /* the version it went back on ("": none) */
    bool again;              /* asked again (plink_fw_again): a rollback of target its FW_STATE
                              * names from before holds nothing */
    uint32_t told_sid;       /* the session INFO named told in (0: none) */
    char told[PLINK_FW_VERSION_MAX + 1];
} plink_fw_mem_t;

typedef struct {
    char target[PLINK_FW_VERSION_MAX + 1];  /* V: the master's own update, downloaded and checked,
                                             * else the version it runs ("": none) */
    char running[PLINK_FW_VERSION_MAX + 1]; /* the version it runs, confirmed ("": on its
                                             * self-test): INFO names it */
    char server[PLINK_FW_SERVER_MAX + 1];   /* its update server, channel and key, named in */
    char channel[PLINK_FW_CHANNEL_MAX + 1]; /* FETCH and RELAY (LINK-036) */
    uint8_t key[PLINK_FW_KEY_LEN];
    bool own_restart;        /* the master's own restart is allowed now (everything disarmed) */
    plink_fw_image_t own;    /* its image of V: running or spare slot (src NONE: none) */
    plink_fw_copy_t copy;
    plink_fw_kp_t kp[PLINK_KEYPADS_MAX];   /* by number: kp[d - 1] */
    plink_fw_mem_t mem[PLINK_KEYPADS_MAX]; /* the last step's */
} plink_fw_world_t;

/* A keypad's row on the master's Software update (local: never on the wire). */
typedef enum {
    PLINK_FWR_NONE = 0,    /* no updates through the link (no FW bit, or its facts not heard) */
    PLINK_FWR_UP_TO_DATE,  /* "Up to date" */
    PLINK_FWR_DOWNLOADING, /* "Downloading 45 %" */
    PLINK_FWR_RECEIVING,   /* "Receiving from this panel 45 %" */
    PLINK_FWR_READY,       /* "Ready · restarts with this panel" */
    PLINK_FWR_RESTARTING,  /* "Restarting" */
    PLINK_FWR_NOT_UPDATED, /* "Not updated yet · tries again by itself" */
    PLINK_FWR_NEWER,       /* newer than this panel (it went back by cable): left alone */
} plink_fw_row_state_t;

typedef struct {
    uint8_t state;   /* plink_fw_row_state_t */
    uint8_t percent; /* DOWNLOADING, RECEIVING */
    uint8_t why;     /* NOT_UPDATED: the last failure's why (0: none) */
} plink_fw_row_t;

typedef struct {
    /* send[d - 1].op != 0: an order for keypad d, everything but its id (plink_master_fw_order
     * numbers it) */
    plink_fw_order_t send[PLINK_KEYPADS_MAX];
    plink_fw_mem_t mem[PLINK_KEYPADS_MAX]; /* keep for the next step */
    plink_fw_row_t row[PLINK_KEYPADS_MAX];
    /* The copy wanted now (copy.version "": none): one held with other facts, or none wanted,
     * is freed; one wanted and not held is fetched. */
    plink_fw_facts_t copy;
    uint8_t installing;      /* bit d-1: keypad d has an INSTALL not yet echoed */
} plink_fw_step_t;

/* One step at now (out is written whole; w is only read). */
void plink_fw_plan(const plink_fw_world_t *w, uint64_t now_ms, plink_fw_step_t *out);

/* Whether image im (of version) can be relayed to a keypad with these facts. */
bool plink_fw_fits(const plink_fw_image_t *im, const char *version, const plink_fw_info_t *info);

/* Ask again now (the console's plink fw update <n>): a wait, a refusal and a going back
 * forgotten - a rollback of the target its FW_STATE still names included (one after this INSTALL
 * still holds it). */
void plink_fw_again(plink_fw_mem_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_FW_H */
