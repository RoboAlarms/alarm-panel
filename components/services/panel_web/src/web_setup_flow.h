#pragma once
/* BL-048: setup/recovery policy shared by display and headless firmware.
 * One service task owns this object. Authentication and atomic durable writes
 * belong to its adapter, never to JSON supplied by a browser. No secrets here.
 * This controller alone does not enable a listener or grant alarm authority. */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WEB_SETUP_UNCLAIMED = 0,
    WEB_SETUP_KEY_PENDING,
    WEB_SETUP_NETWORK,
    WEB_SETUP_READY
} web_setup_stage_t;

typedef struct {
    uint32_t revision;
    web_setup_stage_t stage;
    bool recovery_key_pending;
    bool network_confirmed;
} web_setup_record_t;

typedef enum {
    WEB_SETUP_ACCOUNTS,
    WEB_SETUP_KEY_SAVED,
    WEB_SETUP_FINISH,
    WEB_SETUP_RESET_PASSWORD,
    WEB_SETUP_NETWORK_CONFIRMED
} web_setup_write_t;

/* Must commit the next record AND the operation's account/key changes as one
 * recoverable transaction. false leaves the previous transaction authoritative.
 * The adapter validates/hashes passwords, binds account roles, rotates a recovery
 * key and revokes sessions; the controller never handles their plaintext. */
typedef bool (*web_setup_commit_fn)(void *ctx, web_setup_write_t operation,
                                    const web_setup_record_t *next);

typedef struct {
    bool provisioned;    /* unique claim secret and verified TLS identity exist */
    bool safe_to_change; /* every partition disarmed, no alarm/programming */
    bool authenticated;
    bool may_network; /* freshly checked existing role, not client-selected */
    bool recovery_key_verified;
    bool on_trial_network; /* confirmed from the new interface, not setup AP */
    uint64_t session;      /* nonzero, server-generated session identity */
} web_setup_actor_t;

typedef enum {
    WEB_SETUP_OK = 0,
    WEB_SETUP_UNAVAILABLE,
    WEB_SETUP_LOCKED,
    WEB_SETUP_UNAUTHORIZED,
    WEB_SETUP_BUSY,
    WEB_SETUP_STALE,
    WEB_SETUP_WRONG_STAGE,
    WEB_SETUP_STORAGE
} web_setup_result_t;

typedef struct {
    web_setup_record_t record;
    web_setup_commit_fn commit;
    void *ctx;
    uint64_t claimant, claim_until, recovery_until, trial_owner, trial_until;
} web_setup_flow_t;

/* Raw input is sampled by a board adapter after startup. Require an initial
 * release, debounce both edges, then hold five seconds and release. Poll at
 * least every 100 ms; a longer sampling gap cancels an unobserved gesture. */
typedef struct {
    uint64_t changed_at, pressed_at, sampled_at;
    bool sampled, raw, stable, released;
} web_setup_button_t;
bool web_setup_button_sample(web_setup_button_t *button, bool pressed, uint64_t now);

/* Display panels keep ordinary Panel Manager off until explicitly enabled.
 * Only a provisioned, screen-free master may expose the restricted setup
 * listener automatically. This never grants ordinary web/alarm access. */
bool web_setup_listener_allowed(const web_setup_flow_t *s, bool headless_master, bool provisioned,
                                uint64_t now);

bool web_setup_flow_init(web_setup_flow_t *s, const web_setup_record_t *stored,
                         web_setup_commit_fn commit, void *ctx);
web_setup_result_t web_setup_claim(web_setup_flow_t *s, const web_setup_actor_t *actor,
                                   bool secret_verified, uint64_t now);
web_setup_result_t web_setup_write(web_setup_flow_t *s, const web_setup_actor_t *actor,
                                   web_setup_write_t op, uint32_t revision, uint64_t now);
/* Called only by the verified physical input adapter, never an HTTP endpoint. */
void web_setup_recovery_open(web_setup_flow_t *s, uint64_t now);
bool web_setup_recovery_active(const web_setup_flow_t *s, uint64_t now);
web_setup_result_t web_setup_trial_begin(web_setup_flow_t *s, const web_setup_actor_t *actor,
                                         uint64_t now);
web_setup_result_t web_setup_trial_confirm(web_setup_flow_t *s, const web_setup_actor_t *actor,
                                           uint64_t now);
/* The trial's owner gives it up (a wrong Wi-Fi password, another network): the adapter rolls the
 * network back at once and a new trial may begin. */
web_setup_result_t web_setup_trial_cancel(web_setup_flow_t *s, const web_setup_actor_t *actor);
/* true requires the network adapter to roll back; cannot be renewed by polling.
 * Boot must independently roll back any durable unconfirmed network trial. */
bool web_setup_tick(web_setup_flow_t *s, uint64_t now);
