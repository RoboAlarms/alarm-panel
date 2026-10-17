/*
 * plink_fw.h - the master's side of updates through the link: the planner (docs/features/11
 * "Updates through the link": "Keeping keypads on the master's version", "Choosing the path, per
 * keypad", "The master"; LINK-030, LINK-031, LINK-035).
 *
 * plink_fw_plan is a pure function, run from the master service's poll. It sees the world: the
 * master's own update state and its running image, when its own update check was last answered,
 * the holds the service keeps in NVS link/fwgo, the Try again now requests, the copy the master
 * may hold for keypads of other facts, each keypad as the link's master core sees it
 * (plink_master_fw_keypad: its FW_INFO, its FW_STATE, the order outstanding) and the planner's own
 * memory from the last call. It returns that memory for the next call, the orders to send now
 * (plink_master_fw_order numbers, sends and resends them), the holds as they are now (stored when
 * they changed), and what the service must do with the copy.
 *
 * The rules (decided 2026-10-17: keypads never fall behind, with no person's step):
 *   - The target is always the version the master runs, once confirmed: every keypad behind it is
 *     brought to it - after the master's own update is confirmed, and whenever one is found behind
 *     (newly paired, back on line, its FW_INFO older). There is no rollout to start. Orders go only
 *     while the master runs no update of its own, isn't on its self-test and is quiet
 *     (proto_fw_install_allowed() on its snapshot), to a keypad whose live session has
 *     PLINK_CAP_FW on frames of 250 B or more, heard within 30 s. A keypad on the target or newer
 *     is left alone (never older, OTA-014).
 *   - PROBE comes first (a keypad that can't fetch goes to the relay at once; one whose answer was
 *     lost - echoed, then silent 15 s - is asked again). Its answer picks the path (LINK-031):
 *     FETCH when its server offers exactly the target; BLOCKED when the server refuses the keypad
 *     (10-12, 16-19, or an unknown 10-19); the relay from the master's own slot only for
 *     UNKNOWN_VERSION (15: the server has no release of the target for it, never a copy); the
 *     relay when it isn't reached (1-9; a transfer reason 20-29 or no answer within 2 min - 10 min
 *     while it says it still probes - reads as that) or offers another version or nothing newer
 *     (13, 14, or OK naming another). A waiting reason (50-59: busy, not quiet) is no answer - it
 *     never asked its server, and the relay never goes before the server's word: probed again
 *     after 2 min, nothing counted. The relay comes from the master's own slot (the same device,
 *     a chip revision within the image's range, a project the keypad's ota_profile_accepts, a size
 *     within its slot), else from a copy fetched with its facts while the master reaches its
 *     server - one copy at a time, for a keypad heard (one unheard neither takes it nor keeps it),
 *     and a copy larger than its slot blocks it (TOO_BIG) -, else BLOCKED(NO_IMAGE). A FETCH that
 *     fails for a reach or transfer reason, or whose
 *     check finds another version, falls back to the relay once per target; a refusal of the image
 *     (30-49) never does.
 *   - Concurrency: PROBE and FETCH go to every keypad at once; one RELAY receives at a time (the
 *     waiting keypad with the lowest number next; one whose new key is accepted while another
 *     holds the turn - or a second one taken up at a master restart - waits for it again, its run
 *     cancelled meanwhile); one keypad installs at a time - the READY keypad with the lowest
 *     number, with its facts of this session (no hold has judged one without), once no other holds
 *     the turn (INSTALL ordered, installing, on its self-test, said busy by the core, or heard and
 *     answering while the CANCEL or INFO that voids an INSTALL it took is still on its way). An
 *     INSTALL is never given up while it may still be taken: one unanswered keeps the turn -
 *     resent by the core - for up to 24 h, then a CANCEL replaces it (a keypad away that long comes
 *     back in a new session, which holds no INSTALL); one taken whose keypad's gate goes on saying
 *     no while the master is quiet (a reason the master doesn't see end) keeps it for 24 h of the
 *     master's quiet, then a CANCEL voids it (fetched or relayed again, nothing counted); a relay
 *     whose keypad is unheard for 31 min gives its turn back (its own run has timed out by then).
 *   - The INSTALL hold is of the keypad's session (LINK-034, plink_keypad.h): a keypad still READY
 *     in a new session (a rekey; a master restart is one too) is sent INSTALL again at its turn;
 *     one taken up after a master restart holds none from before.
 *   - Holds, each ending by itself (and, but for BUDGET, KEY_WAIT and the keypad's gate, at Try
 *     again now):
 *       stop (at the keypad, or plink_fw_stop at the master): 24 h, or the master's next version;
 *       a transfer failure: tried again after 2 min; after 3 failed runs in 24 h (the keypad's
 *         BUDGET), once those 24 h have passed; a BUDGET refusal then, or hourly while this
 *         master doesn't know all 3 (a refused order costs nothing); a relay that timed out
 *         paused at its gate (armed, an alarm: its FW_STATE said so) isn't a failed run, and is
 *         tried again 2 min on once the master is quiet - arming never uses up its tries; a READY
 *         image the keypad discarded after 24 h without its turn isn't one either (fetched or
 *         relayed again, no fall-back used up);
 *       BLOCKED: probed again every 24 h, when the master's own check is answered (it reaches its
 *         server again), and when the keypad's facts change;
 *       canary: a keypad that rolled back, or wasn't back on the target within 10 min of
 *         installing, holds its device and chip revision on its version until the master's next
 *         version - never tried again by itself for the same version, so no keypad restarts in a
 *         loop. Try again now sends the keypad that went back first (P2 decision 13) when it is
 *         heard and still behind, else the lowest of the model heard and behind - one unheard is
 *         never ordered - the others held until it is UPDATED (pressed again while that keypad
 *         can't go on, unheard or no longer behind and not installing, another is picked). The
 *         hold ends early only on news: a keypad of the model that didn't return in time, or its
 *         Try again now, found on the target with its self-test passed (its word asked for when
 *         lost); one that passed before another went back ends nothing. A NO_RETURN undone (it
 *         never restarted) leaves a hold another keypad of the model went back under;
 *       KEY_WAIT: the keypad's own, until its new key is accepted there (its run goes on) or the
 *         master's next version;
 *       a refusal of an order or the image (30-49, but BUDGET): until the keypad's facts change or
 *         the master's next version (no retry, "Messages" why table).
 *     Stops and canaries are kept in plink_fw_holds_t (NVS link/fwgo), so a master restart doesn't
 *     undo them; the rest is the planner's memory (RAM). A stop holds a keypad whatever its FW_INFO
 *     (a relay taken up blind); the canary, by its FW_INFO.
 *   - UPDATED only on the keypad's word: back on the target with its FW_STATE's last section
 *     saying its self-test of the target passed (UPDATED, that version; probation over). A keypad
 *     whose service hasn't told its core yet says nothing of the kind, so it stays on its
 *     self-test, and 10 min from installing it is NO_RETURN. A keypad UPDATED that turns up behind
 *     the target again went back; and the last section is read in every session of a keypad
 *     behind: one that rolled back from the target holds its model (after a master restart, say)
 *     - but not one Try again now sent on again, whose rollback of before it is.
 *   - A keypad told to install went back only on its word that it restarted into the target and
 *     left it: its last section saying it rolled back from the target, or, once it ran the target,
 *     its FW_INFO naming another version in a newer session. Its FW_STATE in a new session that
 *     shows it never restarted - still READY (INSTALL again), its image discarded or its run ended
 *     (FAILED, STOPPED: it goes on from there) - holds nothing; nor does a core restarted on its
 *     old version that was never seen installing, its last section naming no rollback of the
 *     target (given, or still not 10 min on): probed again, nothing counted. Taken (echoed) and
 *     then unheard, an INSTALL counts as installing (10 min to NO_RETURN); only a FW_STATE since,
 *     from a keypad heard, puts it back to READY. At NO_RETURN the turn goes on, so an INFO voids
 *     the INSTALL it may still hold (any order taken after an INSTALL voids it), heard or not; a
 *     NO_RETURN keypad heard again whose FW_STATE shows it never restarted undoes its model's hold
 *     and goes on (a router down 10 min while it waited at its gate holds nothing), its turn only
 *     once that INFO is echoed. Whether it restarted is judged on that INSTALL alone (installing
 *     seen on an earlier try says nothing of it).
 *   - CANCEL goes to a keypad with a run under way (not yet installing) that the planner doesn't
 *     follow: the master's own update started or it is on its self-test, a stop, a canary, a
 *     server's refusal answered late, or a run for another version; and at once, heard or not,
 *     for an INSTALL still unanswered that the planner no longer follows; again when the keypad
 *     refused it BUSY (its restart under way) and its restart was then put off (READY again, still
 *     holding its INSTALL). Nothing is cancelled while the master merely isn't quiet: the keypad's
 *     run pauses by itself (HOLD).
 *   - INFO names the master's version, the one a keypad may not pass (LINK-029): in each session
 *     of each keypad with FW (an order of that session naming the version does as well) and again
 *     when the confirmed version changes - never over an order still unanswered, to a keypad told
 *     to install, nor to one probing. The core goes on serving a relay across an INFO.
 *   - For a minute after the master starts (start_ms) nothing new is ordered, not even INFO: runs
 *     are only taken up from the keypads' FW_STATEs (a relay is sent again at once, so it doesn't
 *     stall); a run's FW_STATE comes every 10 s, so within that minute even a lost one is
 *     repeated.
 *   - After a master restart (fresh memory) a keypad's run for the target is taken up from its
 *     FW_STATE - or from the echo of its PROBE, when that FW_STATE was lost: a relay of an image
 *     the master holds is sent again with a new id (it continues by id8) - one at KEY_WAIT once its
 *     key is accepted (RECEIVING), as the new core holds no RELAY for it -, a fetch goes on by
 *     itself, READY waits for its turn, installing is followed. Two runs are taken up even before
 *     the keypad's FW_INFO (lost, say): one installing (it holds the install turn), and a relay of
 *     an image the master holds, known by id8 and size (sent again before it stalls).
 *   - A probe is waited for while its keypad's FW_STATE says it is still probing (up to 10 min),
 *     else 2 min; its answer still counts once the relay went: a server's refusal (10-12, 16-19)
 *     blocks it there and cancels the relay, never relayed past the server's rules (LINK-031).
 *   - A keypad sends FW_INFO once a session, and a run's end (FAILED, STOPPED) once. One whose
 *     live session hasn't brought its FW_INFO (lost) is asked INFO every 10 s - before it is
 *     probed, and once a keypad told to install shows up in a new session; so is one heard whose
 *     run's FW_STATE (every 10 s) is a minute late, and one with a run heard in a newer session
 *     without a FW_STATE there. Nothing is judged on an earlier session's facts or a stale run
 *     meanwhile, and no RELAY goes before the answer (it would start a stopped run again): a lost
 *     frame never reads as a keypad that didn't come back (NO_RETURN would hold its whole model),
 *     nor as a run that broke off, nor leaves one holding the install or relay turn. A run is gone
 *     (BROKE_OFF) when its keypad's FW_STATE says it knows no order and runs nothing (it
 *     restarted), or 30 s in a newer session with none.
 *
 * Times: every time in the world, the holds and the tracks is the master's clock (now_ms, as the
 * link's core runs on). A stop is kept as the time it ends, which is always ahead of now while it
 * holds: the service stores it in link/fwgo as wall-clock time (or as the time left while its
 * clock isn't set) and hands in now plus what is left - without a set clock, 24 h from the
 * restart.
 *
 * Portable C11, no heap, no globals; everything is the caller's memory.
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

#define PLINK_FWP_PROBE_WAIT_MS 120000u   /* a PROBE unanswered this long reads as not reached, */
#define PLINK_FWP_PROBE_MAX_MS  600000u   /* ... this long while its keypad says it still probes */
#define PLINK_FWP_PROBE_LOST_MS 15000u    /* a PROBE echoed, then no FW_STATE this long (one comes
                                           * every 10 s while it probes): its answer was lost -
                                           * asked again */
#define PLINK_FWP_RETRY_MS      120000u   /* a transfer failure is tried again after this, ... */
#define PLINK_FWP_BUDGET        3u        /* ... and after this many failed runs ... */
#define PLINK_FWP_BUDGET_MS     86400000u /* ... in this long (its BUDGET), once that passed */
#define PLINK_FWP_BUDGET_ASK_MS 3600000u  /* a BUDGET refusal whose 3 runs this master doesn't all
                                           * know: asked again so often */
#define PLINK_FWP_HOLD_MS       86400000u /* a stop; a blocked keypad probed again; an INSTALL
                                           * unanswered keeps the turn; one taken, its gate saying
                                           * no while the master is quiet, keeps it */
#define PLINK_FWP_RETURN_MS     600000u   /* back on the target within this of installing */
#define PLINK_FWP_RELAY_AWAY_MS 1860000u  /* a relay's keypad unheard this long gives the turn back
                                           * (its run times out after 30 min paused) */
#define PLINK_FWP_COPY_IDLE_MS  1800000u  /* a copy unused this long is freed */
#define PLINK_FWP_INFO_MS       10000u    /* INFO again when no FW_INFO came this long */
#define PLINK_FWP_LATE_MS       60000u    /* a run's FW_STATE (every 10 s) this late though its
                                           * keypad is heard: asked for (INFO) */
#define PLINK_FWP_MOVED_MS      30000u    /* in a newer session with no FW_STATE there this long,
                                           * though asked: its run is gone (BROKE_OFF) */
#define PLINK_FWP_SETTLE_MS     60000u    /* after the master starts, runs are only taken up this
                                           * long (a keypad may hold an INSTALL from before) */
#define PLINK_FWP_CHECK_MS      86400000u /* the master reaches its server: a check answered so
                                           * recently */
#define PLINK_FWP_PAGE          1024u     /* a RELAY's page, count and gap (each side clamps the */
#define PLINK_FWP_COUNT         16u       /* page to its frame): 16 KB a request on UDP */
#define PLINK_FWP_GAP_MS        0u
#define PLINK_FWP_CANARIES      PLINK_KEYPADS_MAX /* models held at once (one per keypad at most) */

/* A keypad as the master's link sees it (plink_master_fw_keypad fills it). */
typedef struct {
    uint8_t device;          /* its number (0: no keypad here) */
    bool fw_cap;             /* its live session holds PLINK_CAP_FW */
    bool fw;                 /* ... on frames of 250 B or more: orders go */
    bool online;             /* a live session, heard within the last 30 s */
    uint32_t sessions;       /* moves with each new session (a restart shows as one) */
    bool have_info;          /* a FW_INFO came since this master started ... */
    bool info_now;           /* ... in its live session */
    plink_fw_info_t info;
    uint64_t info_ms;
    bool have_state;         /* a FW_STATE came since this master started ... */
    bool state_now;          /* ... in its live session */
    plink_fw_state_t state;
    uint64_t state_ms;       /* when it came */
    uint32_t order;          /* the order outstanding (0: none), resent until a FW_STATE echoes */
    uint8_t order_op;        /* ... its op */
    bool order_acked;        /* ... echoed, ... */
    uint8_t order_why;       /* ... with this why (its refusal when it was refused) */
    uint64_t order_ms;       /* ... given then */
    bool away;               /* an order unanswered 30 s, a run's FW_STATE missing 30 s, or not
                              * heard for 30 s */
    bool busy;               /* installing or on its self-test: supervision held (up to 10 min a
                              * session) - it holds the install turn */
} plink_fw_kp_t;

/* A keypad's place, as the master shows it (local: never on the wire). */
typedef enum {
    PLINK_FWK_UNKNOWN = 0, /* no FW_INFO yet */
    PLINK_FWK_CURRENT,     /* on the master's version or newer: left alone */
    PLINK_FWK_BEHIND,      /* older: its PROBE goes once orders may */
    PLINK_FWK_PROBING,     /* PROBE sent: its server's answer awaited */
    PLINK_FWK_FETCH,       /* fetching from its own server */
    PLINK_FWK_RELAY_WAIT,  /* waits for the relay's turn, its image or a copy */
    PLINK_FWK_RELAY,       /* receiving from this master */
    PLINK_FWK_KEY_WAIT,    /* "Accept the new key at keypad n" */
    PLINK_FWK_READY,       /* the image is in and checked: waits for its turn to install */
    PLINK_FWK_INSTALLING,  /* restarting into it */
    PLINK_FWK_CHECKING,    /* back on it, its self-test running */
    PLINK_FWK_UPDATED,
    PLINK_FWK_FAILED,      /* why (a plink_fw_why_t, or one of PLINK_FWK_WHY_*) */
    PLINK_FWK_BLOCKED,     /* why */
    PLINK_FWK_STOPPED,     /* why: STOPPED (a person at the keypad) or CANCELLED (the master) */
} plink_fwk_state_t;

/* The planner's own reasons (local: never on the wire), above every plink_fw_why_t. */
#define PLINK_FWK_WHY_NO_IMAGE    200u /* not reached, and no image for it here: "Connect keypad n
                                        * to the internet, or update it with a cable" */
#define PLINK_FWK_WHY_CANARY      201u /* a keypad of its device and chip revision went back */
#define PLINK_FWK_WHY_NO_RETURN   202u /* not back on the target within 10 min of installing */
#define PLINK_FWK_WHY_ROLLED_BACK 203u /* its self-test failed: it went back */

/* How it gets the image. */
typedef enum {
    PLINK_FWK_PATH_NONE = 0,
    PLINK_FWK_PATH_FETCH,      /* from its own update server */
    PLINK_FWK_PATH_RELAY_OWN,  /* relayed from the master's running slot */
    PLINK_FWK_PATH_RELAY_COPY, /* relayed from a copy fetched with its facts */
} plink_fwk_path_t;

/* The master's own update (plink_fw_world_t.own). */
typedef enum {
    PLINK_FWM_IDLE = 0,  /* confirmed, no update of its own running */
    PLINK_FWM_UPDATING,  /* its own update runs */
    PLINK_FWM_SELF_TEST, /* on its self-test, not yet confirmed */
} plink_fwm_t;

/* The copy's state (plink_fw_copy_t.state). */
typedef enum {
    PLINK_FWCOPY_NONE = 0,
    PLINK_FWCOPY_FETCHING,
    PLINK_FWCOPY_READY,
    PLINK_FWCOPY_FAILED, /* why: the server's refusal, the image's, or a reach or transfer reason */
} plink_fwcopy_t;

/* An image the master can relay. */
typedef struct {
    char version[PLINK_FW_VERSION_MAX + 1];
    char device[PLINK_FW_DEVICE_MAX + 1];
    char project[PLINK_FW_PROJECT_MAX + 1];
    uint32_t size;
    uint8_t sha256[PLINK_SHA256_LEN]; /* its first 8 bytes are the relay's id8 */
    uint8_t key[PLINK_FW_KEY_LEN];    /* the key it is signed with (zeros: not known) */
} plink_fw_image_t;

/* The master's running image (panel_ota_running_image, worked out once after confirmation). */
typedef struct {
    plink_fw_image_t image;    /* version, device and project always; the rest once have */
    bool have;                 /* size, sha256, key and the chip range are worked out */
    uint16_t chip_min, chip_max; /* the chip revisions it runs on (chip_rev_full, inclusive) */
} plink_fw_own_t;

/* A copy fetched with one keypad's facts (panel_ota_fetch_for): one at a time. */
typedef struct {
    uint8_t state;             /* plink_fwcopy_t */
    uint8_t why;               /* FAILED: a plink_fw_why_t */
    char device[PLINK_FW_DEVICE_MAX + 1];   /* the facts it was fetched with */
    char chip_rev[PLINK_FW_CHIP_MAX + 1];
    char project[PLINK_FW_PROJECT_MAX + 1];
    plink_fw_image_t image;    /* READY: what it holds (image.version the target) */
    uint64_t used_ms;          /* fetched, or last read for a relay */
} plink_fw_copy_t;

/* A model held on its version (the canary): its device and chip revision ("": a free entry). */
typedef struct {
    char device[PLINK_FW_DEVICE_MAX + 1];
    char chip_rev[PLINK_FW_CHIP_MAX + 1];
    uint8_t retry; /* Try again now: keypad n tries the version again first, the model's others
                    * held until it is UPDATED (0: none; its going back again holds them all) */
} plink_fw_canary_t;

/* The holds that outlive a master restart (the service keeps them in NVS link/fwgo and hands them
 * in; the planner returns them as they are now). Every hold belongs to version, the master's
 * version when it was made: once the master runs another (its own update under way counts as
 * running the old one), all of them end. A stop is kept as the time it ends (see Times above). */
typedef struct {
    char version[PLINK_FW_VERSION_MAX + 1];
    uint64_t stop_until_ms[PLINK_KEYPADS_MAX];  /* keypad d stopped until then (0: not) */
    plink_fw_canary_t canary[PLINK_FWP_CANARIES];
} plink_fw_holds_t;

/* The planner's memory of one keypad (the caller keeps it between calls; zeros to start). */
typedef struct {
    char target[PLINK_FW_VERSION_MAX + 1]; /* the version it is for: another starts it again */
    uint8_t state;             /* plink_fwk_state_t */
    uint8_t why;               /* FAILED, BLOCKED, STOPPED: a plink_fw_why_t or PLINK_FWK_WHY_* */
    uint8_t path;              /* plink_fwk_path_t */
    uint8_t op;                /* the last order asked for (0: none) ... */
    uint64_t op_ms;            /* ... then */
    uint64_t since_ms;         /* in this state since */
    bool own_only;             /* its relay only from the master's own slot (UNKNOWN_VERSION) */
    bool fell_back;            /* its FETCH fell back to the relay (once per target) */
    bool installed;            /* told to install: what comes back is judged as an install */
    bool tried;                /* ... and its FW_STATE said INSTALLING since it last said READY:
                                * it restarted into the target (a keypad never seen so, back on
                                * another version, went back only by its last section's word) */
    uint8_t canary_made;       /* what its going back did to its model's hold (0 nothing, 1 made
                                * it, 2 ended its Try again now): undone when a NO_RETURN turns
                                * out never to have restarted - but a hold another keypad of the
                                * model went back under stays (a rollback keeps it; another
                                * NO_RETURN takes it over) */
    bool retried;              /* Try again now sent it on again: a rollback from before in its
                                * last section holds nothing (until it is told to install) */
    bool budget;               /* FAILED: waits for the keypad's BUDGET (3 failed runs in 24 h),
                                * which Try again now doesn't end */
    uint32_t sessions;         /* INSTALL ordered (or installing seen) in its session numbered so */
    uint32_t last_sessions;    /* the session whose FW_STATE's last section was read (0: none) */
    uint32_t told;             /* the session the master's version was named in (0: none) */
    uint32_t facts;            /* its FW_INFO's facts when it was held: a change ends the hold */
    uint64_t retry_at;         /* FAILED: tried again then (0: no retry by time) */
    uint64_t fails[PLINK_FWP_BUDGET]; /* when its last failed runs ended (0: none) */
    uint64_t away_ms;          /* RELAY: unheard since (0: heard) */
    uint64_t moved_ms;         /* first heard in a newer session than its run's, no FW_STATE
                                * there yet (0: not) */
    uint64_t probe_ms;         /* its PROBE went then (0: none): an answer made since counts */
    bool gate_paused;          /* RELAY: its newest FW_STATE said PAUSED at its gate (a why of the
                                * waiting class but NO_STATUS): a TIMEOUT then isn't a failed run */
    uint64_t again_ms;         /* the newest Try again now it has taken */
    uint32_t voids;            /* its order (a CANCEL or INFO) took the place of an INSTALL it took,
                                * or of a CANCEL it refused BUSY, in its session numbered so (0:
                                * none): until that order is echoed there, it may still restart on
                                * the INSTALL - it holds the install turn */
    uint64_t gate_quiet_ms;    /* READY, its INSTALL taken: how long the master has been quiet while
                                * its gate said no, this turn (24 h: given up, CANCEL) ... */
    uint64_t gate_at;          /* ... counted up to then (0: not counting) */
} plink_fw_track_t;

typedef struct {
    uint64_t start_ms;         /* this master started (its link's core, and the planner's memory) */
    uint8_t own;               /* plink_fwm_t */
    bool quiet;                /* proto_fw_install_allowed() passes on the master's snapshot */
    uint64_t check_ms;         /* its own update check last answered (0: none since start); it
                                * reaches its server for 24 h after */
    char server[PLINK_FW_SERVER_MAX + 1];   /* its server and channel, named in PROBE and FETCH */
    char channel[PLINK_FW_CHANNEL_MAX + 1];
    plink_fw_own_t run;        /* the image it runs: run.image.version is the target */
    plink_fw_copy_t copy;
    plink_fw_holds_t holds;    /* link/fwgo */
    /* Try again now (the master's Software update, plink fw update [n|all]): the time of its
     * newest request for keypad d (0: none). It ends the master's holds at once, never a
     * keypad's BUDGET, KEY_WAIT or gate. */
    uint64_t again_ms[PLINK_KEYPADS_MAX];
    plink_fw_kp_t kp[PLINK_KEYPADS_MAX];        /* by number: kp[d - 1] */
    plink_fw_track_t track[PLINK_KEYPADS_MAX];  /* the last step's tracks */
} plink_fw_world_t;

typedef struct {
    plink_fw_track_t track[PLINK_KEYPADS_MAX];  /* keep for the next call */
    /* send[d - 1].op != 0: an order for keypad d, everything but its id (plink_master_fw_order
     * numbers it, and it supersedes the one outstanding) */
    plink_fw_order_t send[PLINK_KEYPADS_MAX];
    plink_fw_holds_t holds;    /* the holds now: hand them in next time ... */
    bool holds_changed;        /* ... and store them in link/fwgo when they changed */
    uint8_t copy_for;          /* fetch a copy of the target with keypad n's facts (0: none) */
    bool copy_free;            /* the copy held (or being fetched) serves no keypad: free it */
    uint8_t running;           /* bit d-1: keypad d is being updated (Stop updating keypads) */
    uint8_t held;              /* bit d-1: keypad d waits on a hold Try again now ends */
} plink_fw_step_t;

/* One step at now (out is written whole; w is only read). */
void plink_fw_plan(const plink_fw_world_t *w, uint64_t now_ms, plink_fw_step_t *out);

/* Whether the master's running image serves keypad info by relay (the own slot rules above). */
bool plink_fw_own_fits(const plink_fw_own_t *run, const plink_fw_info_t *info);

/* Stop updating keypad device (1..8; 0: every keypad) at the master (OTA-027): held until 24 h
 * from now_ms, or until the master's next version (version: the one it runs). The next step
 * cancels its run unless it is installing. */
void plink_fw_stop(plink_fw_holds_t *h, uint8_t device, const char *version, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PLINK_FW_H */
