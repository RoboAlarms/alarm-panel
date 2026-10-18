#include "web_setup_flow.h"
#include <stddef.h>
#include <string.h>

#define CLAIM_MS    UINT64_C(300000)
#define RECOVERY_MS UINT64_C(300000)
#define TRIAL_MS    UINT64_C(120000)

static uint64_t deadline(uint64_t now, uint64_t duration)
{
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

bool web_setup_flow_init(web_setup_flow_t *s, const web_setup_record_t *stored,
                         web_setup_commit_fn commit, void *ctx)
{
    if (!s || !stored || !commit || stored->stage < WEB_SETUP_UNCLAIMED ||
        stored->stage > WEB_SETUP_READY || stored->revision == UINT32_MAX ||
        (stored->stage == WEB_SETUP_UNCLAIMED &&
         (stored->recovery_key_pending || stored->network_confirmed)) ||
        (stored->stage == WEB_SETUP_KEY_PENDING && !stored->recovery_key_pending) ||
        (stored->stage == WEB_SETUP_READY && !stored->network_confirmed))
        return false;
    memset(s, 0, sizeof *s);
    s->record = *stored;
    s->commit = commit;
    s->ctx = ctx;
    return true;
}

bool web_setup_button_sample(web_setup_button_t *b, bool pressed, uint64_t now)
{
    if (!b->sampled || now < b->sampled_at || now - b->sampled_at > 100) {
        memset(b, 0, sizeof *b);
        b->sampled = true;
        b->raw = b->stable = pressed;
        b->changed_at = b->sampled_at = now;
        return false;
    }
    b->sampled_at = now;
    if (pressed != b->raw) {
        b->raw = pressed;
        b->changed_at = now;
    }
    if (now - b->changed_at < 50)
        return false;
    if (!b->released) {
        if (!pressed)
            b->released = true;
        b->stable = pressed;
        return false;
    }
    if (b->stable == pressed)
        return false;
    b->stable = pressed;
    if (pressed) {
        b->pressed_at = b->changed_at;
        return false;
    }
    return b->changed_at - b->pressed_at >= 5000;
}

bool web_setup_listener_allowed(const web_setup_flow_t *s, bool headless_master, bool provisioned,
                                uint64_t now)
{
    return headless_master && provisioned &&
           (s->record.stage != WEB_SETUP_READY || web_setup_recovery_active(s, now));
}

static web_setup_result_t gate(const web_setup_actor_t *a)
{
    if (!a || !a->provisioned)
        return WEB_SETUP_UNAVAILABLE;
    if (!a->safe_to_change)
        return WEB_SETUP_LOCKED;
    if (!a->session)
        return WEB_SETUP_UNAUTHORIZED;
    return WEB_SETUP_OK;
}

web_setup_result_t web_setup_claim(web_setup_flow_t *s, const web_setup_actor_t *a, bool verified,
                                   uint64_t now)
{
    web_setup_result_t r = gate(a);
    if (r != WEB_SETUP_OK)
        return r;
    if (s->record.stage != WEB_SETUP_UNCLAIMED)
        return WEB_SETUP_WRONG_STAGE;
    if (!verified)
        return WEB_SETUP_UNAUTHORIZED;
    if (s->claimant && now < s->claim_until) {
        return s->claimant == a->session ? WEB_SETUP_OK : WEB_SETUP_BUSY;
    }
    s->claimant = a->session;
    s->claim_until = deadline(now, CLAIM_MS);
    return WEB_SETUP_OK;
}

void web_setup_recovery_open(web_setup_flow_t *s, uint64_t now)
{
    if (!web_setup_recovery_active(s, now))
        s->recovery_until = deadline(now, RECOVERY_MS);
}

bool web_setup_recovery_active(const web_setup_flow_t *s, uint64_t now)
{
    return s->recovery_until && now < s->recovery_until;
}

web_setup_result_t web_setup_write(web_setup_flow_t *s, const web_setup_actor_t *a,
                                   web_setup_write_t op, uint32_t revision, uint64_t now)
{
    web_setup_result_t r = gate(a);
    if (r != WEB_SETUP_OK)
        return r;
    if (revision != s->record.revision || revision == UINT32_MAX - 1u)
        return WEB_SETUP_STALE;
    web_setup_record_t next = s->record;
    switch (op) {
    case WEB_SETUP_ACCOUNTS:
        if (next.stage != WEB_SETUP_UNCLAIMED)
            return WEB_SETUP_WRONG_STAGE;
        if (s->claimant != a->session || now >= s->claim_until)
            return WEB_SETUP_UNAUTHORIZED;
        next.stage = WEB_SETUP_KEY_PENDING;
        next.recovery_key_pending = true;
        break;
    case WEB_SETUP_KEY_SAVED:
        if (!next.recovery_key_pending)
            return WEB_SETUP_WRONG_STAGE;
        if (!a->authenticated || !a->may_network)
            return WEB_SETUP_UNAUTHORIZED;
        next.recovery_key_pending = false;
        if (next.stage == WEB_SETUP_KEY_PENDING)
            next.stage = WEB_SETUP_NETWORK;
        break;
    case WEB_SETUP_FINISH:
        if (next.stage != WEB_SETUP_NETWORK)
            return WEB_SETUP_WRONG_STAGE;
        if (!a->authenticated || !a->may_network)
            return WEB_SETUP_UNAUTHORIZED;
        if (next.recovery_key_pending || !next.network_confirmed || s->trial_until)
            return WEB_SETUP_STALE;
        next.stage = WEB_SETUP_READY;
        break;
    case WEB_SETUP_RESET_PASSWORD:
        if (next.stage == WEB_SETUP_UNCLAIMED)
            return WEB_SETUP_WRONG_STAGE;
        if (s->trial_until)
            return WEB_SETUP_BUSY;
        if (!web_setup_recovery_active(s, now) || !a->recovery_key_verified)
            return WEB_SETUP_UNAUTHORIZED;
        /* The replacement key must be acknowledged before completing setup. */
        next.recovery_key_pending = true;
        if (next.stage != WEB_SETUP_READY)
            next.stage = WEB_SETUP_KEY_PENDING;
        break;
    default: return WEB_SETUP_WRONG_STAGE;
    }
    next.revision++;
    if (!s->commit(s->ctx, op, &next))
        return WEB_SETUP_STORAGE;
    s->record = next;
    s->claimant = s->claim_until = 0;
    if (op == WEB_SETUP_RESET_PASSWORD)
        s->recovery_until = 0;
    return WEB_SETUP_OK;
}

web_setup_result_t web_setup_trial_begin(web_setup_flow_t *s, const web_setup_actor_t *a,
                                         uint64_t now)
{
    web_setup_result_t r = gate(a);
    if (r != WEB_SETUP_OK)
        return r;
    if (s->record.stage != WEB_SETUP_NETWORK && s->record.stage != WEB_SETUP_READY)
        return WEB_SETUP_WRONG_STAGE;
    if (!a->authenticated || !a->may_network)
        return WEB_SETUP_UNAUTHORIZED;
    if (s->record.recovery_key_pending)
        return WEB_SETUP_WRONG_STAGE;
    if (s->trial_until)
        return WEB_SETUP_BUSY; /* caller must roll back expired trial first */
    s->trial_owner = a->session;
    s->trial_until = deadline(now, TRIAL_MS);
    return WEB_SETUP_OK;
}

web_setup_result_t web_setup_trial_confirm(web_setup_flow_t *s, const web_setup_actor_t *a,
                                           uint64_t now)
{
    web_setup_result_t r = gate(a);
    if (r != WEB_SETUP_OK)
        return r;
    if (!a->authenticated || !a->may_network || !a->on_trial_network)
        return WEB_SETUP_UNAUTHORIZED;
    if (!s->trial_until || now >= s->trial_until || s->trial_owner != a->session)
        return WEB_SETUP_STALE;
    if (s->record.revision >= UINT32_MAX - 1u)
        return WEB_SETUP_STALE;
    web_setup_record_t next = s->record;
    next.network_confirmed = true;
    next.revision++;
    /* Adapter confirms the durable network trial in the same recoverable
     * transaction. Failure keeps the rollback deadline and current record. */
    if (!s->commit(s->ctx, WEB_SETUP_NETWORK_CONFIRMED, &next))
        return WEB_SETUP_STORAGE;
    s->record = next;
    s->trial_until = 0;
    return WEB_SETUP_OK;
}

bool web_setup_tick(web_setup_flow_t *s, uint64_t now)
{
    if (s->claimant && now >= s->claim_until)
        s->claimant = s->claim_until = 0;
    if (s->recovery_until && now >= s->recovery_until)
        s->recovery_until = 0;
    if (!s->trial_until || now < s->trial_until)
        return false;
    s->trial_until = s->trial_owner = 0;
    return true;
}
