/*
 * config.c - configuration defaults, validation and zone type rules.
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_config.h"

static void copy_name(char *dst, const char *src)
{
    size_t n = 0;
    if (src != NULL) {
        while (n + 1u < ALARM_NAME_LEN && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    memset(dst + n, 0, ALARM_NAME_LEN - n);
}

void alarm_config_defaults(alarm_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = ALARM_CONFIG_VERSION;

    alarm_options_t *o = &cfg->opt;
    o->code_min_len = ALARM_CODE_MIN_LEN;
    o->abort_window_s = 30;
    o->cancel_window_s = 300;
    o->bell_timeout_s = 240;
    o->fire_bell_timeout = false;
    o->swinger_trips = 2;
    o->cross_zone_window_s = 300;
    o->fire_verify_window_s = 90;
    o->smoke_reset_s = 6;
    o->exit_restart_limit = 1;
    o->recent_closing_s = 120;
    o->auto_stay = true;
    o->exit_sounds = true;
    o->quick_arm = false;
    o->quick_exit = true;
    o->remote_arm_without_code = false;
    o->power_up_previous_state = true;
    o->power_up_settle_s = 60;
    o->lockout_attempts = 5;
    o->lockout_s = 300;
    o->walk_test_timeout_s = 3600;
    /* Everything beeps except AC loss (outages are common) and clock not set. */
    o->trouble_beep_mask = ((1u << ALARM_TRBL_COUNT) - 1u) &
                           ~((1u << ALARM_TRBL_AC_LOSS) | (1u << ALARM_TRBL_CLOCK_NOT_SET));
    o->panic_keys[0] = ALARM_PANIC_POLICE;
    o->panic_keys[1] = ALARM_PANIC_MEDICAL;
    o->panic_keys[2] = ALARM_PANIC_FIRE;
    o->relaxed_limits = false;
    o->power_up_program = true;
    o->chime_needs_code = true;
    o->auto_less_secure = true; /* AUT-015 / D25: on by default, the installer may turn it off */

    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        alarm_partition_config_t *p = &cfg->partitions[i];
        p->enabled = (i == 0);
        char name[ALARM_NAME_LEN];
        snprintf(name, sizeof(name), "Partition %u", i + 1u);
        copy_name(p->name, name);
        p->exit_delay_s = 60;
        p->entry_delay1_s = 30;
        p->entry_delay2_s = 45;
    }
}

void alarm_zone_config_defaults(alarm_zone_config_t *z, uint16_t zone_no, alarm_zone_type_t type,
                                uint8_t partition)
{
    memset(z, 0, sizeof(*z));
    z->type = type;
    z->partition = partition;

    switch (type) {
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
        z->flags = ALARM_ZF_CHIME;
        z->device_class = ALARM_DEVCLASS_DOOR;
        break;
    case ALARM_ZT_PERIMETER:
    case ALARM_ZT_SILENT_BURGLARY:
        z->flags = ALARM_ZF_CHIME;
        z->device_class = ALARM_DEVCLASS_WINDOW;
        break;
    case ALARM_ZT_INTERIOR_FOLLOWER:
    case ALARM_ZT_INTERIOR_DELAY:
    case ALARM_ZT_DAY_NIGHT:
        z->device_class = ALARM_DEVCLASS_MOTION;
        break;
    case ALARM_ZT_FIRE:
    case ALARM_ZT_FIRE_VERIFIED:
        z->device_class = ALARM_DEVCLASS_SMOKE;
        break;
    case ALARM_ZT_CO:
        z->device_class = ALARM_DEVCLASS_CO;
        break;
    case ALARM_ZT_24H_SILENT:
    case ALARM_ZT_24H_AUDIBLE:
    case ALARM_ZT_24H_AUX:
        z->device_class = ALARM_DEVCLASS_PANIC_BUTTON;
        break;
    case ALARM_ZT_ARM_STAY:
    case ALARM_ZT_ARM_AWAY:
    case ALARM_ZT_DISARM:
        z->device_class = ALARM_DEVCLASS_KEYFOB;
        break;
    case ALARM_ZT_KEYSWITCH:
        z->device_class = ALARM_DEVCLASS_KEYSWITCH;
        break;
    default:
        z->device_class = ALARM_DEVCLASS_GENERIC;
        break;
    }

    char name[ALARM_NAME_LEN];
    snprintf(name, sizeof(name), "Zone %03u", (unsigned)zone_no);
    copy_name(z->name, name);
}

void alarm_config_set_user(alarm_config_t *cfg, uint8_t user_no, alarm_authority_t authority,
                           uint8_t partitions, const char *name,
                           const uint8_t hash[ALARM_CODE_HASH_LEN])
{
    if (user_no < 1u || user_no > ALARM_MAX_USERS) {
        return;
    }
    alarm_user_t *u = &cfg->users[user_no - 1u];
    memset(u, 0, sizeof(*u));
    u->authority = authority;
    u->partitions = partitions;
    copy_name(u->name, name);
    if (hash != NULL) {
        memcpy(u->code_hash, hash, ALARM_CODE_HASH_LEN);
    }
}

void alarm_config_set_default_users(alarm_config_t *cfg, alarm_hash_fn hash, void *hash_ctx)
{
    const uint8_t all = (uint8_t)((1u << ALARM_MAX_PARTITIONS) - 1u);
    uint8_t h[ALARM_CODE_HASH_LEN];

    hash(hash_ctx, cfg->code_salt, ALARM_DEFAULT_INSTALLER_CODE, h);
    alarm_config_set_user(cfg, ALARM_USER_INSTALLER, ALARM_AUTH_INSTALLER, all, "Installer", h);
    hash(hash_ctx, cfg->code_salt, ALARM_DEFAULT_MASTER_CODE, h);
    alarm_config_set_user(cfg, ALARM_USER_MASTER, ALARM_AUTH_MASTER, all, "Master", h);
}

bool alarm_zone_type_valid(int type)
{
    switch (type) {
    case ALARM_ZT_NOT_USED:
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
    case ALARM_ZT_PERIMETER:
    case ALARM_ZT_INTERIOR_FOLLOWER:
    case ALARM_ZT_DAY_NIGHT:
    case ALARM_ZT_24H_SILENT:
    case ALARM_ZT_24H_AUDIBLE:
    case ALARM_ZT_24H_AUX:
    case ALARM_ZT_FIRE:
    case ALARM_ZT_INTERIOR_DELAY:
    case ALARM_ZT_MONITOR:
    case ALARM_ZT_CO:
    case ALARM_ZT_FIRE_VERIFIED:
    case ALARM_ZT_ARM_STAY:
    case ALARM_ZT_ARM_AWAY:
    case ALARM_ZT_DISARM:
    case ALARM_ZT_NO_RESPONSE:
    case ALARM_ZT_SILENT_BURGLARY:
    case ALARM_ZT_MOMENTARY_EXIT:
    case ALARM_ZT_KEYSWITCH:
        return true;
    default:
        return false;
    }
}

bool alarm_zone_type_is_burglary(alarm_zone_type_t type)
{
    switch (type) {
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
    case ALARM_ZT_PERIMETER:
    case ALARM_ZT_INTERIOR_FOLLOWER:
    case ALARM_ZT_DAY_NIGHT:
    case ALARM_ZT_INTERIOR_DELAY:
    case ALARM_ZT_SILENT_BURGLARY:
        return true;
    default:
        return false;
    }
}

bool alarm_zone_type_is_fire(alarm_zone_type_t type)
{
    return type == ALARM_ZT_FIRE || type == ALARM_ZT_FIRE_VERIFIED;
}

bool alarm_zone_type_is_24h(alarm_zone_type_t type)
{
    return type == ALARM_ZT_24H_SILENT || type == ALARM_ZT_24H_AUDIBLE ||
           type == ALARM_ZT_24H_AUX;
}

bool alarm_zone_armed_in_level(const alarm_zone_config_t *z, alarm_arm_level_t level)
{
    switch (z->type) {
    case ALARM_ZT_ENTRY_EXIT_1:
    case ALARM_ZT_ENTRY_EXIT_2:
    case ALARM_ZT_PERIMETER:
    case ALARM_ZT_SILENT_BURGLARY:
        return level == ALARM_LEVEL_AWAY || level == ALARM_LEVEL_STAY ||
               level == ALARM_LEVEL_NIGHT;
    case ALARM_ZT_INTERIOR_FOLLOWER:
    case ALARM_ZT_INTERIOR_DELAY:
    case ALARM_ZT_DAY_NIGHT:
        return level == ALARM_LEVEL_AWAY ||
               (level == ALARM_LEVEL_NIGHT && (z->flags & ALARM_ZF_NIGHT) != 0u);
    default:
        return false;
    }
}

bool alarm_zone_bypassable(const alarm_zone_config_t *z)
{
    if (alarm_zone_type_is_burglary(z->type)) {
        return true;
    }
    if (alarm_zone_type_is_24h(z->type)) {
        return (z->flags & ALARM_ZF_BYPASSABLE) != 0u;
    }
    return false; /* fire, CO, monitor, buttons, keyswitch... */
}

bool alarm_zone_config_resets_state(const alarm_zone_config_t *before,
                                    const alarm_zone_config_t *after)
{
    return before->type != after->type || before->partition != after->partition;
}

/* Each problem in the English the console, logs and MQTT show: "<what> <num>: <problem>", or just
 * "<problem>" when what is NULL. The touchscreen words the codes itself (docs/features/37). */
static const struct {
    const char *what;
    const char *problem;
} ENGLISH[ALARM_CFG_PROBLEM_COUNT] = {
    [ALARM_CFG_FINE] = {NULL, ""},
    [ALARM_CFG_NULL] = {NULL, "config is NULL"},
    [ALARM_CFG_VERSION] = {"config version", "unsupported"},
    [ALARM_CFG_CODE_LENGTH] = {"code_min_len", "out of range 4..10"},
    [ALARM_CFG_ABORT_WINDOW] = {"abort_window_s", "> 45"},
    [ALARM_CFG_CANCEL_WINDOW] = {"cancel_window_s", "< 300"},
    [ALARM_CFG_SWINGER] = {"swinger_trips", "out of range 1..6"},
    [ALARM_CFG_FIRE_TIMES] = {NULL, "fire verify window and smoke reset must be > 0"},
    [ALARM_CFG_PANIC_KEY] = {"panic key", "invalid"},
    [ALARM_CFG_PARTITION_NAME] = {"partition", "name not terminated"},
    [ALARM_CFG_EXIT_DELAY] = {"partition", "exit delay out of range 45..255"},
    [ALARM_CFG_ENTRY_DELAY] = {"partition", "entry delay out of range 30..240"},
    [ALARM_CFG_DELAY_TOO_LONG] = {"partition", "delay > 600 s"},
    [ALARM_CFG_NO_PARTITION] = {NULL, "no partition enabled"},
    [ALARM_CFG_ZONE_TYPE] = {"zone", "has an invalid type"},
    [ALARM_CFG_ZONE_NAME] = {"zone", "name not terminated"},
    [ALARM_CFG_ZONE_PARTITION] = {"zone", "assigned to a disabled partition"},
    [ALARM_CFG_ZONE_CROSS] = {"zone", "has an invalid cross zone"},
    [ALARM_CFG_ZONE_USER] = {"zone", "(button/keyswitch) needs a user number"},
    [ALARM_CFG_USER_AUTHORITY] = {"user", "has an invalid authority"},
    [ALARM_CFG_USER_NAME] = {"user", "name not terminated"},
    [ALARM_CFG_USER_INSTALLER] = {"user", "must be the installer"},
    [ALARM_CFG_USER_MASTER] = {"user", "must be the system master"},
    [ALARM_CFG_USER_ROLE] = {"user", "cannot be installer/system master"},
    [ALARM_CFG_USER_PARTITIONS] = {"user", "has no partitions"},
    [ALARM_CFG_USER_SAME_CODE] = {"user", "shares a code with another user"},
    [ALARM_CFG_DEVICE_NAME] = {"device", "name not terminated"},
    [ALARM_CFG_DEVICE_PARTITION] = {"device", "has an invalid partition"},
    [ALARM_CFG_ZONE_WORDS] = {"zone", "has an invalid spoken name"},
};

static alarm_result_t fail(alarm_config_why_t *why, alarm_config_problem_t problem, unsigned num)
{
    if (why != NULL) {
        why->problem = problem;
        why->number = num;
    }
    return ALARM_ERR_BAD_ARG;
}

static bool name_ok(const char *name)
{
    return memchr(name, '\0', ALARM_NAME_LEN) != NULL;
}

alarm_result_t alarm_config_validate(const alarm_config_t *cfg, char *why, size_t why_len)
{
    alarm_config_why_t w;
    const alarm_result_t r = alarm_config_check(cfg, &w);
    if (why != NULL && why_len > 0u) {
        const unsigned p = (unsigned)w.problem;
        if (r == ALARM_OK || p >= (unsigned)ALARM_CFG_PROBLEM_COUNT) {
            why[0] = '\0';
        } else if (ENGLISH[p].what != NULL) {
            snprintf(why, why_len, "%s %u: %s", ENGLISH[p].what, w.number, ENGLISH[p].problem);
        } else {
            snprintf(why, why_len, "%s", ENGLISH[p].problem);
        }
    }
    return r;
}

alarm_result_t alarm_config_check(const alarm_config_t *cfg, alarm_config_why_t *why)
{
    if (why != NULL) {
        why->problem = ALARM_CFG_FINE;
        why->number = 0u;
    }
    if (cfg == NULL) {
        return fail(why, ALARM_CFG_NULL, 0u);
    }
    if (cfg->version != ALARM_CONFIG_VERSION) {
        return fail(why, ALARM_CFG_VERSION, cfg->version);
    }
    const alarm_options_t *o = &cfg->opt;
    const bool strict = !o->relaxed_limits;

    if (o->code_min_len < ALARM_CODE_MIN_LEN || o->code_min_len > ALARM_CODE_MAX_LEN) {
        return fail(why, ALARM_CFG_CODE_LENGTH, o->code_min_len);
    }
    if (strict && o->abort_window_s > 45u) {
        return fail(why, ALARM_CFG_ABORT_WINDOW, o->abort_window_s);
    }
    if (strict && o->cancel_window_s < 300u) {
        return fail(why, ALARM_CFG_CANCEL_WINDOW, o->cancel_window_s);
    }
    if (o->swinger_trips < 1u || (strict && o->swinger_trips > 6u)) {
        return fail(why, ALARM_CFG_SWINGER, o->swinger_trips);
    }
    if (o->fire_verify_window_s == 0u || o->smoke_reset_s == 0u) {
        return fail(why, ALARM_CFG_FIRE_TIMES, 0u);
    }
    for (unsigned k = 0; k < 3u; k++) {
        if ((unsigned)o->panic_keys[k] > (unsigned)ALARM_PANIC_MEDICAL) {
            return fail(why, ALARM_CFG_PANIC_KEY, k);
        }
    }

    bool any_partition = false;
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        const alarm_partition_config_t *p = &cfg->partitions[i];
        if (!name_ok(p->name)) {
            return fail(why, ALARM_CFG_PARTITION_NAME, i + 1u);
        }
        if (!p->enabled) {
            continue;
        }
        any_partition = true;
        if (strict && (p->exit_delay_s < 45u || p->exit_delay_s > 255u)) {
            return fail(why, ALARM_CFG_EXIT_DELAY, i + 1u);
        }
        if (strict && (p->entry_delay1_s < 30u || p->entry_delay1_s > 240u ||
                       p->entry_delay2_s < 30u || p->entry_delay2_s > 240u)) {
            return fail(why, ALARM_CFG_ENTRY_DELAY, i + 1u);
        }
        if (p->exit_delay_s > 600u || p->entry_delay1_s > 600u || p->entry_delay2_s > 600u) {
            return fail(why, ALARM_CFG_DELAY_TOO_LONG, i + 1u);
        }
    }
    if (!any_partition) {
        return fail(why, ALARM_CFG_NO_PARTITION, 0u);
    }

    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        const alarm_zone_config_t *z = &cfg->zones[i];
        const unsigned zn = i + 1u;
        if (!alarm_zone_type_valid((int)z->type)) {
            return fail(why, ALARM_CFG_ZONE_TYPE, zn);
        }
        if (!name_ok(z->name)) {
            return fail(why, ALARM_CFG_ZONE_NAME, zn);
        }
        if (z->type == ALARM_ZT_NOT_USED) {
            continue;
        }
        if (z->partition < 1u || z->partition > ALARM_MAX_PARTITIONS ||
            !cfg->partitions[z->partition - 1u].enabled) {
            return fail(why, ALARM_CFG_ZONE_PARTITION, zn);
        }
        if (z->cross_zone != 0u &&
            (z->cross_zone > ALARM_MAX_ZONES || z->cross_zone == zn ||
             cfg->zones[z->cross_zone - 1u].type == ALARM_ZT_NOT_USED)) {
            return fail(why, ALARM_CFG_ZONE_CROSS, zn);
        }
        const bool button = z->type == ALARM_ZT_ARM_STAY || z->type == ALARM_ZT_ARM_AWAY ||
                            z->type == ALARM_ZT_DISARM || z->type == ALARM_ZT_KEYSWITCH;
        if (button && (z->user < 1u || z->user > ALARM_MAX_USERS)) {
            return fail(why, ALARM_CFG_ZONE_USER, zn);
        }
        if (!alarm_zone_words_valid(z)) {
            return fail(why, ALARM_CFG_ZONE_WORDS, zn); /* CORE-CFG-020 */
        }
    }

    for (unsigned i = 0; i < ALARM_MAX_USERS; i++) {
        const alarm_user_t *u = &cfg->users[i];
        const unsigned un = i + 1u;
        if ((unsigned)u->authority > (unsigned)ALARM_AUTH_DURESS) {
            return fail(why, ALARM_CFG_USER_AUTHORITY, un);
        }
        if (!name_ok(u->name)) {
            return fail(why, ALARM_CFG_USER_NAME, un);
        }
        if (u->authority == ALARM_AUTH_NONE) {
            continue;
        }
        if (un == ALARM_USER_INSTALLER && u->authority != ALARM_AUTH_INSTALLER) {
            return fail(why, ALARM_CFG_USER_INSTALLER, un);
        }
        if (un == ALARM_USER_MASTER && u->authority != ALARM_AUTH_MASTER) {
            return fail(why, ALARM_CFG_USER_MASTER, un);
        }
        if (un > ALARM_USER_MASTER &&
            (u->authority == ALARM_AUTH_INSTALLER || u->authority == ALARM_AUTH_MASTER)) {
            return fail(why, ALARM_CFG_USER_ROLE, un);
        }
        if (u->partitions == 0u) {
            return fail(why, ALARM_CFG_USER_PARTITIONS, un);
        }
        for (unsigned j = 0; j < i; j++) {
            const alarm_user_t *o2 = &cfg->users[j];
            if (o2->authority != ALARM_AUTH_NONE &&
                memcmp(o2->code_hash, u->code_hash, ALARM_CODE_HASH_LEN) == 0) {
                return fail(why, ALARM_CFG_USER_SAME_CODE, un);
            }
        }
    }

    for (unsigned i = 0; i < ALARM_MAX_DEVICES; i++) {
        const alarm_device_config_t *d = &cfg->devices[i];
        if (!name_ok(d->name)) {
            return fail(why, ALARM_CFG_DEVICE_NAME, i + 1u);
        }
        if (d->partition > ALARM_MAX_PARTITIONS) {
            return fail(why, ALARM_CFG_DEVICE_PARTITION, i + 1u);
        }
    }

    return ALARM_OK;
}

/* ---- A zone's spoken name and its revisions (CORE-CFG-020, CORE-CFG-021) ------------------- */

uint16_t alarm_zone_word(const alarm_zone_config_t *zone, unsigned i)
{
    if (zone == NULL || i >= ALARM_ZONE_WORDS) {
        return 0u;
    }
    const uint8_t *p = &zone->spoken_words[2u * i];
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

void alarm_zone_set_word(alarm_zone_config_t *zone, unsigned i, uint16_t token)
{
    if (zone == NULL || i >= ALARM_ZONE_WORDS) {
        return;
    }
    zone->spoken_words[2u * i] = (uint8_t)(token & 0xFFu);
    zone->spoken_words[2u * i + 1u] = (uint8_t)(token >> 8);
}

unsigned alarm_zone_word_count(const alarm_zone_config_t *zone)
{
    unsigned n = 0u;
    while (n < ALARM_ZONE_WORDS && alarm_zone_word(zone, n) != 0u) {
        n++;
    }
    return n;
}

bool alarm_zone_words_valid(const alarm_zone_config_t *zone)
{
    if (zone == NULL) {
        return false;
    }
    const unsigned n = alarm_zone_word_count(zone);
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        const uint16_t t = alarm_zone_word(zone, i);
        if (i < n ? t > ALARM_VOICE_TOKEN_SELECTABLE_MAX : t != 0u) {
            return false; /* a word that can't be chosen, or one after a hole */
        }
    }
    return true;
}

bool alarm_zone_voice_approved(const alarm_zone_config_t *zone)
{
    return zone != NULL && zone->type != ALARM_ZT_NOT_USED && zone->voice_binding_revision != 0u &&
           zone->voice_approved_revision == zone->voice_binding_revision &&
           zone->voice_pack_revision != 0u && alarm_zone_word_count(zone) > 0u &&
           alarm_zone_words_valid(zone);
}

/* A new binding revision, never reused; 0 once the counter is exhausted (it never wraps). */
static uint32_t voice_next_revision(uint32_t *counter)
{
    if (*counter == UINT32_MAX) {
        return 0u;
    }
    return ++*counter;
}

static void voice_clear_approval(alarm_zone_config_t *z)
{
    z->voice_approved_revision = 0u;
    z->voice_pack_revision = 0u;
}

void alarm_zone_voice_reconcile(const alarm_zone_config_t *old, alarm_zone_config_t *next,
                                uint32_t *counter)
{
    if (old == NULL || next == NULL || counter == NULL) {
        return;
    }
    if (next->type == ALARM_ZT_NOT_USED) {
        /* D01: Not used removes the spoken name and its approval; the counter survives. */
        memset(next->spoken_words, 0, sizeof(next->spoken_words));
        next->voice_binding_revision = 0u;
        voice_clear_approval(next);
        return;
    }
    /* The identity is the slot in use, its display name and its partition (the sensor is
     * alarm_config_voice_invalidate_zone's). A client asking for a different binding revision
     * than the stored one asks for a new identity: it may only lose approval that way. */
    const bool same_identity = old->type != ALARM_ZT_NOT_USED &&
                               old->partition == next->partition &&
                               strncmp(old->name, next->name, ALARM_NAME_LEN) == 0 &&
                               old->voice_binding_revision == next->voice_binding_revision;
    if (!same_identity) {
        next->voice_binding_revision = voice_next_revision(counter);
        voice_clear_approval(next);
        return;
    }
    const bool words_changed =
        memcmp(old->spoken_words, next->spoken_words, sizeof(old->spoken_words)) != 0;
    uint32_t binding = old->voice_binding_revision;
    if (binding == 0u && words_changed) {
        binding = voice_next_revision(counter); /* the first voice edit of a migrated zone */
    }
    next->voice_binding_revision = binding;
    if (words_changed) {
        voice_clear_approval(next); /* a new recipe is approved by its own transaction */
        return;
    }
    /* The same identity and words: the stored approval stands, whatever the writer sent. Only
     * alarm_config_voice_approve/_withdraw move it (D07: a client's approval is never trusted,
     * so a replayed old configuration can't bring one back). */
    next->voice_approved_revision = old->voice_approved_revision;
    next->voice_pack_revision = old->voice_pack_revision;
}

void alarm_config_voice_reconcile(const alarm_config_t *old, alarm_config_t *next)
{
    if (old == NULL || next == NULL) {
        return;
    }
    uint32_t counter = old->opt.voice_revision_counter;
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        alarm_zone_voice_reconcile(&old->zones[i], &next->zones[i], &counter);
    }
    next->opt.voice_revision_counter = counter;
}

alarm_voice_result_t alarm_config_voice_approve(alarm_config_t *cfg, uint16_t zone_no,
                                                uint32_t expected_binding,
                                                const uint16_t words[ALARM_ZONE_WORDS],
                                                uint32_t pack_revision)
{
    if (cfg == NULL || words == NULL || zone_no < 1u || zone_no > ALARM_MAX_ZONES ||
        pack_revision == 0u) {
        return ALARM_VOICE_BAD_ARG;
    }
    alarm_zone_config_t *z = &cfg->zones[zone_no - 1u];
    if (z->type == ALARM_ZT_NOT_USED) {
        return ALARM_VOICE_BAD_ARG;
    }
    if (z->voice_binding_revision == 0u || cfg->opt.voice_revision_counter == UINT32_MAX) {
        return ALARM_VOICE_UNAVAILABLE;
    }
    if (z->voice_binding_revision != expected_binding) {
        return ALARM_VOICE_STALE;
    }
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        if (alarm_zone_word(z, i) != words[i]) {
            return ALARM_VOICE_STALE; /* not the recipe the person was shown */
        }
    }
    if (alarm_zone_word_count(z) == 0u || !alarm_zone_words_valid(z)) {
        return ALARM_VOICE_BAD_ARG;
    }
    z->voice_approved_revision = z->voice_binding_revision;
    z->voice_pack_revision = pack_revision;
    return ALARM_VOICE_OK;
}

alarm_voice_result_t alarm_config_voice_withdraw(alarm_config_t *cfg, uint16_t zone_no,
                                                 uint32_t expected_binding)
{
    if (cfg == NULL || zone_no < 1u || zone_no > ALARM_MAX_ZONES ||
        cfg->zones[zone_no - 1u].type == ALARM_ZT_NOT_USED) {
        return ALARM_VOICE_BAD_ARG;
    }
    alarm_zone_config_t *z = &cfg->zones[zone_no - 1u];
    if (z->voice_binding_revision != expected_binding) {
        return ALARM_VOICE_STALE;
    }
    voice_clear_approval(z);
    return ALARM_VOICE_OK;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t voice_highest_revision(const alarm_config_t *cfg)
{
    uint32_t hi = cfg->opt.voice_revision_counter;
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        hi = max_u32(hi, cfg->zones[i].voice_binding_revision);
        hi = max_u32(hi, cfg->zones[i].voice_approved_revision);
    }
    return hi;
}

void alarm_config_voice_restore(const alarm_config_t *current, alarm_config_t *restored)
{
    if (current == NULL || restored == NULL) {
        return;
    }
    uint32_t counter = max_u32(voice_highest_revision(current), voice_highest_revision(restored));
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        alarm_zone_config_t *z = &restored->zones[i];
        voice_clear_approval(z);
        if (z->type == ALARM_ZT_NOT_USED) {
            memset(z->spoken_words, 0, sizeof(z->spoken_words));
            z->voice_binding_revision = 0u;
        } else {
            z->voice_binding_revision = voice_next_revision(&counter);
        }
    }
    restored->opt.voice_revision_counter = counter;
}

alarm_result_t alarm_config_voice_invalidate_zone(alarm_config_t *cfg, uint16_t zone_no)
{
    if (cfg == NULL || zone_no < 1u || zone_no > ALARM_MAX_ZONES ||
        cfg->zones[zone_no - 1u].type == ALARM_ZT_NOT_USED) {
        return ALARM_ERR_BAD_ARG;
    }
    alarm_zone_config_t *z = &cfg->zones[zone_no - 1u];
    z->voice_binding_revision = voice_next_revision(&cfg->opt.voice_revision_counter);
    voice_clear_approval(z);
    return ALARM_OK;
}

unsigned alarm_config_voice_invalidate_zones(alarm_config_t *cfg, const uint8_t *zones)
{
    unsigned n = 0u;
    if (cfg == NULL || zones == NULL) {
        return 0u;
    }
    for (uint16_t z = 1u; z <= ALARM_MAX_ZONES; z++) {
        if ((zones[(z - 1u) / 8u] & (1u << ((z - 1u) % 8u))) != 0u &&
            alarm_config_voice_invalidate_zone(cfg, z) == ALARM_OK) {
            n++;
        }
    }
    return n;
}
