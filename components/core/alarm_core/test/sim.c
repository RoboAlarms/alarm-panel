/*
 * sim.c - scenario harness implementation (see sim.h).
 */
#include "sim.h"

#include <string.h>

/* ---- recording ports ---------------------------------------------------- */
static void on_event(void *ctx, const alarm_event_t *ev)
{
    sim_t *s = ctx;
    if (s->n_events < SIM_MAX_RECORDS) {
        s->events[s->n_events++] = *ev;
    }
}

static void on_report(void *ctx, const alarm_event_t *ev)
{
    sim_t *s = ctx;
    if (s->n_reports < SIM_MAX_RECORDS) {
        s->reports[s->n_reports++] = *ev;
    }
}

static void on_status(void *ctx, uint8_t partition)
{
    sim_t *s = ctx;
    if (partition <= ALARM_MAX_PARTITIONS) {
        s->status_calls[partition]++;
    }
}

static void on_sound(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone)
{
    sim_t *s = ctx;
    if (s->n_sounds < SIM_MAX_RECORDS) {
        sim_sound_t *r = &s->sounds[s->n_sounds++];
        r->partition = partition;
        r->sound = sound;
        r->zone = zone;
        r->at = s->now;
    }
}

static void on_output(void *ctx, alarm_output_t output, uint8_t partition, alarm_pattern_t pat)
{
    sim_t *s = ctx;
    if (s->n_outputs < SIM_MAX_RECORDS) {
        sim_output_t *r = &s->outputs[s->n_outputs++];
        r->output = output;
        r->partition = partition;
        r->pattern = pat;
        r->at = s->now;
    }
}

static void on_persist(void *ctx, const alarm_runtime_state_t *st)
{
    sim_t *s = ctx;
    s->persisted = *st;
    s->n_persist++;
}

static void on_config_changed(void *ctx)
{
    sim_t *s = ctx;
    s->n_config_changed++;
}

static int64_t get_epoch_ms(void *ctx)
{
    const sim_t *s = ctx;
    return (int64_t)(1790000000000ull + s->now); /* a fixed "wall clock" */
}

/* FNV-1a expanded to 32 bytes. Test-only stand-in for PBKDF2/SHA-256. */
void sim_hash(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
              uint8_t out[ALARM_CODE_HASH_LEN])
{
    (void)ctx;
    for (unsigned block = 0; block < ALARM_CODE_HASH_LEN / 8u; block++) {
        uint64_t h = 1469598103934665603ull ^ (uint64_t)(block * 0x9E3779B97F4A7C15ull);
        for (unsigned i = 0; i < ALARM_CODE_SALT_LEN; i++) {
            h = (h ^ salt[i]) * 1099511628211ull;
        }
        for (const char *c = code; *c != '\0'; c++) {
            h = (h ^ (uint8_t)*c) * 1099511628211ull;
        }
        for (unsigned b = 0; b < 8u; b++) {
            out[block * 8u + b] = (uint8_t)(h >> (8u * b));
        }
    }
}

/* ---- configuration ------------------------------------------------------ */
void sim_set_user(alarm_config_t *cfg, uint8_t user_no, alarm_authority_t authority,
                  uint8_t partitions, const char *code)
{
    uint8_t h[ALARM_CODE_HASH_LEN];
    sim_hash(NULL, cfg->code_salt, code, h);
    alarm_config_set_user(cfg, user_no, authority, partitions, alarm_authority_name(authority),
                          h);
}

void sim_set_zone(alarm_config_t *cfg, uint16_t zone, alarm_zone_type_t type, uint8_t partition)
{
    alarm_zone_config_defaults(&cfg->zones[zone - 1u], zone, type, partition);
}

void sim_config_basic(alarm_config_t *cfg)
{
    alarm_config_defaults(cfg);
    cfg->opt.power_up_settle_s = 0;
    for (unsigned i = 0; i < ALARM_CODE_SALT_LEN; i++) {
        cfg->code_salt[i] = (uint8_t)(0xA5u ^ i);
    }

    static const alarm_zone_type_t layout[] = {
        ALARM_ZT_ENTRY_EXIT_1, ALARM_ZT_ENTRY_EXIT_2,  ALARM_ZT_PERIMETER,
        ALARM_ZT_INTERIOR_FOLLOWER, ALARM_ZT_INTERIOR_DELAY, ALARM_ZT_FIRE,
        ALARM_ZT_FIRE_VERIFIED, ALARM_ZT_CO,           ALARM_ZT_24H_AUDIBLE,
        ALARM_ZT_24H_SILENT,   ALARM_ZT_24H_AUX,       ALARM_ZT_DAY_NIGHT,
        ALARM_ZT_MONITOR,      ALARM_ZT_SILENT_BURGLARY, ALARM_ZT_INTERIOR_FOLLOWER,
        ALARM_ZT_NO_RESPONSE,  ALARM_ZT_ARM_AWAY,      ALARM_ZT_DISARM,
        ALARM_ZT_KEYSWITCH,    ALARM_ZT_MOMENTARY_EXIT,
    };
    for (uint16_t z = 1; z <= sizeof(layout) / sizeof(layout[0]); z++) {
        sim_set_zone(cfg, z, layout[z - 1u], 1);
    }
    cfg->zones[Z_UPSTAIRS - 1].flags |= ALARM_ZF_NIGHT;
    cfg->zones[Z_FOB_AWAY - 1].user = 3;
    cfg->zones[Z_FOB_OFF - 1].user = 3;
    cfg->zones[Z_KEYSWITCH - 1].user = 3;

    alarm_config_set_default_users(cfg, sim_hash, NULL); /* 01 installer, 02 master */
    sim_set_user(cfg, 3, ALARM_AUTH_STANDARD, 0x01, CODE_USER);
    sim_set_user(cfg, 4, ALARM_AUTH_ARM_ONLY, 0x01, CODE_ARMONLY);
    sim_set_user(cfg, 5, ALARM_AUTH_GUEST, 0x01, CODE_GUEST);
    sim_set_user(cfg, 6, ALARM_AUTH_DURESS, 0x01, CODE_DURESS);
    sim_set_user(cfg, 7, ALARM_AUTH_PARTITION_MASTER, 0x01, CODE_PMASTER);
}

/* ---- lifecycle ---------------------------------------------------------- */
void sim_clear_records(sim_t *s)
{
    s->n_events = 0;
    s->n_reports = 0;
    s->n_sounds = 0;
    s->n_outputs = 0;
    memset(s->status_calls, 0, sizeof(s->status_calls));
    s->n_persist = 0;
    s->n_config_changed = 0;
}

void sim_start(sim_t *s, const alarm_config_t *cfg, const alarm_runtime_state_t *restore)
{
    memset(s, 0, sizeof(*s));
    if (cfg != NULL) {
        s->cfg = *cfg;
    } else {
        sim_config_basic(&s->cfg);
    }
    s->now = SEC(1000);
    s->keypad.kind = ALARM_SRC_KEYPAD;
    s->keypad.id = 1;

    alarm_ports_t ports;
    memset(&ports, 0, sizeof(ports));
    ports.ctx = s;
    ports.on_event = on_event;
    ports.on_report = on_report;
    ports.on_status = on_status;
    ports.on_sound = on_sound;
    ports.on_output = on_output;
    ports.on_persist = on_persist;
    ports.on_config_changed = on_config_changed;
    ports.hash_code = sim_hash;
    ports.get_epoch_ms = get_epoch_ms;

    char why[96];
    TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_OK, alarm_config_validate(&s->cfg, why, sizeof(why)), why);
    TEST_ASSERT_EQUAL_INT(ALARM_OK, alarm_engine_init(&s->eng, &s->cfg, &ports, restore, s->now));
}

/* ---- driving ------------------------------------------------------------ */
void sim_advance(sim_t *s, alarm_ms_t ms)
{
    /* Tick at every internal deadline (like the platform timer would). */
    const alarm_ms_t target = s->now + ms;
    for (;;) {
        const alarm_ms_t d = alarm_engine_next_deadline(&s->eng);
        if (d > target) {
            break;
        }
        if (d > s->now) {
            s->now = d;
        }
        alarm_engine_tick(&s->eng, s->now);
        if (alarm_engine_next_deadline(&s->eng) <= s->now) {
            TEST_FAIL_MESSAGE("engine deadline did not advance");
        }
    }
    s->now = target;
    alarm_engine_tick(&s->eng, s->now);
}

void sim_zev(sim_t *s, uint16_t zone, alarm_zone_event_t ev)
{
    alarm_engine_zone_event(&s->eng, zone, ev, s->now);
}

void sim_fault(sim_t *s, uint16_t zone)
{
    sim_zev(s, zone, ALARM_ZEV_FAULT);
}

void sim_restore(sim_t *s, uint16_t zone)
{
    sim_zev(s, zone, ALARM_ZEV_RESTORE);
}

alarm_result_t sim_cmd(sim_t *s, const alarm_cmd_t *cmd)
{
    return alarm_engine_command(&s->eng, cmd, &s->keypad, s->now, &s->last);
}

static void cmd_init(alarm_cmd_t *cmd, alarm_cmd_type_t type, uint8_t p, const char *code)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    cmd->partition = p;
    if (code != NULL) {
        strncpy(cmd->code, code, ALARM_CODE_MAX_LEN);
    }
}

alarm_result_t sim_arm(sim_t *s, uint8_t p, const char *code, alarm_arm_level_t level,
                       uint8_t flags)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_ARM, p, code);
    cmd.level = level;
    cmd.arm_flags = flags;
    return sim_cmd(s, &cmd);
}

alarm_result_t sim_disarm(sim_t *s, uint8_t p, const char *code)
{
    return sim_simple(s, ALARM_CMD_DISARM, p, code);
}

alarm_result_t sim_simple(sim_t *s, alarm_cmd_type_t type, uint8_t p, const char *code)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, type, p, code);
    return sim_cmd(s, &cmd);
}

alarm_result_t sim_bypass(sim_t *s, uint8_t p, const char *code, uint16_t zone)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_BYPASS, p, code);
    cmd.zone = zone;
    return sim_cmd(s, &cmd);
}

alarm_result_t sim_panic(sim_t *s, uint8_t p, alarm_panic_t panic)
{
    alarm_cmd_t cmd;
    cmd_init(&cmd, ALARM_CMD_PANIC, p, NULL);
    cmd.panic = panic;
    return sim_cmd(s, &cmd);
}

void sim_arm_away_and_leave(sim_t *s, const char *code)
{
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_arm(s, 1, code, ALARM_LEVEL_AWAY, 0));
    sim_advance(s, SEC(5));
    sim_fault(s, Z_FRONT);
    sim_advance(s, SEC(3));
    sim_restore(s, Z_FRONT);
    sim_advance(s, SEC(60));
    EXPECT_STATE(s, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(s, 1, ALARM_LEVEL_AWAY);
}

void sim_arm_stay_settled(sim_t *s, const char *code)
{
    TEST_ASSERT_EQUAL_INT(ALARM_OK, sim_arm(s, 1, code, ALARM_LEVEL_STAY, 0));
    sim_advance(s, SEC(61));
    EXPECT_STATE(s, 1, ALARM_STATE_ARMED);
    EXPECT_LEVEL(s, 1, ALARM_LEVEL_STAY);
}

/* ---- queries ------------------------------------------------------------ */
alarm_partition_status_t sim_status(sim_t *s, uint8_t p)
{
    alarm_partition_status_t st;
    alarm_engine_partition_status(&s->eng, p, &st);
    return st;
}

alarm_zone_status_t sim_zstatus(sim_t *s, uint16_t zone)
{
    alarm_zone_status_t st;
    alarm_engine_zone_status(&s->eng, zone, &st);
    return st;
}

static unsigned count(const alarm_event_t *list, unsigned n, alarm_event_type_t type)
{
    unsigned c = 0;
    for (unsigned i = 0; i < n; i++) {
        if (list[i].type == type) {
            c++;
        }
    }
    return c;
}

static const alarm_event_t *last(const alarm_event_t *list, unsigned n, alarm_event_type_t type)
{
    for (unsigned i = n; i > 0; i--) {
        if (list[i - 1].type == type) {
            return &list[i - 1];
        }
    }
    return NULL;
}

unsigned sim_count_events(const sim_t *s, alarm_event_type_t type)
{
    return count(s->events, s->n_events, type);
}

const alarm_event_t *sim_last_event(const sim_t *s, alarm_event_type_t type)
{
    return last(s->events, s->n_events, type);
}

unsigned sim_count_reports(const sim_t *s, alarm_event_type_t type)
{
    return count(s->reports, s->n_reports, type);
}

const alarm_event_t *sim_last_report(const sim_t *s, alarm_event_type_t type)
{
    return last(s->reports, s->n_reports, type);
}

static bool has_alarm(const alarm_event_t *list, unsigned n, uint16_t zone, alarm_cause_t cause)
{
    for (unsigned i = 0; i < n; i++) {
        if (list[i].type == ALARM_EV_ALARM && list[i].zone == zone &&
            list[i].arg == (uint8_t)cause) {
            return true;
        }
    }
    return false;
}

bool sim_alarm_reported(const sim_t *s, uint16_t zone, alarm_cause_t cause)
{
    return has_alarm(s->reports, s->n_reports, zone, cause);
}

bool sim_alarm_logged(const sim_t *s, uint16_t zone, alarm_cause_t cause)
{
    return has_alarm(s->events, s->n_events, zone, cause);
}

unsigned sim_count_sounds(const sim_t *s, alarm_sound_t sound)
{
    unsigned c = 0;
    for (unsigned i = 0; i < s->n_sounds; i++) {
        if (s->sounds[i].sound == sound) {
            c++;
        }
    }
    return c;
}

unsigned sim_count_zone_sounds(const sim_t *s, alarm_sound_t sound, uint16_t zone)
{
    unsigned c = 0;
    for (unsigned i = 0; i < s->n_sounds; i++) {
        if (s->sounds[i].sound == sound && s->sounds[i].zone == zone) {
            c++;
        }
    }
    return c;
}

alarm_pattern_t sim_output(const sim_t *s, alarm_output_t output, uint8_t partition)
{
    for (unsigned i = s->n_outputs; i > 0; i--) {
        const sim_output_t *o = &s->outputs[i - 1];
        if (o->output == output && o->partition == partition) {
            return o->pattern;
        }
    }
    return ALARM_PAT_OFF;
}
