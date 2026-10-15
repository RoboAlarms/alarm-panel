/*
 * sim.h - scenario harness for alarm_core tests: a fake clock plus recording
 * ports around one engine instance.
 *
 * Standard layout (sim_config_basic), partition 1, codes are 4 digits:
 *   zone  1 entry/exit 1  "Front Door"        zone 11 24h aux       "Water Sensor"
 *   zone  2 entry/exit 2  "Garage Door"       zone 12 day/night     "Gun Cabinet"
 *   zone  3 perimeter     "Living Window"     zone 13 monitor       "Freezer Temp"
 *   zone  4 int. follower "Hall Motion"       zone 14 silent burg.  "Shed Door"
 *   zone  5 int. delay    "Family Motion"     zone 15 follower+NIGHT "Upstairs Motion"
 *   zone  6 fire          "Hall Smoke"        zone 16 no response   "Doorbell"
 *   zone  7 fire verified "Bedroom Smoke"     zone 17 arm-away fob  (user 3)
 *   zone  8 CO            "Hall CO"           zone 18 disarm fob    (user 3)
 *   zone  9 24h audible   "Panic Button"      zone 19 keyswitch     (user 3)
 *   zone 10 24h silent    "Hold-up Button"    zone 20 momentary exit
 *
 *   users: 1 installer 69420 and 2 master 123456 (the factory defaults), 3 standard 1111,
 *          4 arm-only 2222,
 *          5 guest 3333, 6 duress 9999, 7 partition master (p1) 5555.
 *   Codes may be 4-10 digits; each user picks their own length.
 *
 * Delays: exit 60 s, entry 1 30 s, entry 2 45 s, abort 30 s, bell 240 s,
 * settle 0 s (so zones react immediately).
 */
#ifndef ALARM_SIM_H
#define ALARM_SIM_H

#include <string.h>

#include "alarm/alarm_engine.h"
#include "alarm/alarm_keyseq.h"
#include "alarm/alarm_strings.h"
#include "unity.h"

#define SIM_MAX_RECORDS 512

#define Z_FRONT     1
#define Z_GARAGE    2
#define Z_WINDOW    3
#define Z_HALL_PIR  4
#define Z_FAMILY    5
#define Z_SMOKE     6
#define Z_SMOKE_VER 7
#define Z_CO        8
#define Z_PANIC     9
#define Z_HOLDUP    10
#define Z_WATER     11
#define Z_CABINET   12
#define Z_FREEZER   13
#define Z_SHED      14
#define Z_UPSTAIRS  15
#define Z_DOORBELL  16
#define Z_FOB_AWAY  17
#define Z_FOB_OFF   18
#define Z_KEYSWITCH 19
#define Z_MOM_EXIT  20

#define CODE_INSTALLER ALARM_DEFAULT_INSTALLER_CODE /* "69420" */
#define CODE_MASTER    ALARM_DEFAULT_MASTER_CODE    /* "123456" */
#define CODE_USER      "1111"
#define CODE_ARMONLY   "2222"
#define CODE_GUEST     "3333"
#define CODE_DURESS    "9999"
#define CODE_PMASTER   "5555"

#define SEC(s) ((alarm_ms_t)(s) * 1000u)

typedef struct {
    uint8_t partition;
    alarm_sound_t sound;
    uint16_t zone;
    alarm_ms_t at;
} sim_sound_t;

typedef struct {
    alarm_output_t output;
    uint8_t partition;
    alarm_pattern_t pattern;
    alarm_ms_t at;
} sim_output_t;

typedef struct {
    alarm_engine_t eng;
    alarm_config_t cfg;
    alarm_ms_t now;
    alarm_source_t keypad; /* default command source: keypad 1 */

    alarm_event_t events[SIM_MAX_RECORDS];
    unsigned n_events;
    alarm_event_t reports[SIM_MAX_RECORDS];
    unsigned n_reports;
    sim_sound_t sounds[SIM_MAX_RECORDS];
    unsigned n_sounds;
    sim_output_t outputs[SIM_MAX_RECORDS];
    unsigned n_outputs;
    unsigned status_calls[ALARM_MAX_PARTITIONS + 1];
    alarm_runtime_state_t persisted;
    unsigned n_persist;
    unsigned n_config_changed;
    alarm_cmd_result_t last;
} sim_t;

/* ---- configuration helpers ---------------------------------------------- */
void sim_hash(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
              uint8_t out[ALARM_CODE_HASH_LEN]);
void sim_config_basic(alarm_config_t *cfg);
void sim_set_user(alarm_config_t *cfg, uint8_t user_no, alarm_authority_t authority,
                  uint8_t partitions, const char *code);
void sim_set_zone(alarm_config_t *cfg, uint16_t zone, alarm_zone_type_t type, uint8_t partition);

/* ---- lifecycle ---------------------------------------------------------- */
/* Starts the engine at t = 1000 s with cfg (or the basic layout when NULL). */
void sim_start(sim_t *s, const alarm_config_t *cfg, const alarm_runtime_state_t *restore);
void sim_clear_records(sim_t *s);

/* ---- driving ------------------------------------------------------------ */
void sim_advance(sim_t *s, alarm_ms_t ms);
void sim_fault(sim_t *s, uint16_t zone);
void sim_restore(sim_t *s, uint16_t zone);
void sim_zev(sim_t *s, uint16_t zone, alarm_zone_event_t ev);
alarm_result_t sim_cmd(sim_t *s, const alarm_cmd_t *cmd);
alarm_result_t sim_arm(sim_t *s, uint8_t p, const char *code, alarm_arm_level_t level,
                       uint8_t flags);
alarm_result_t sim_disarm(sim_t *s, uint8_t p, const char *code);
alarm_result_t sim_simple(sim_t *s, alarm_cmd_type_t type, uint8_t p, const char *code);
alarm_result_t sim_bypass(sim_t *s, uint8_t p, const char *code, uint16_t zone);
alarm_result_t sim_panic(sim_t *s, uint8_t p, alarm_panic_t panic);
/* Arm AWAY and walk out through the front door so the system ends up armed away. */
void sim_arm_away_and_leave(sim_t *s, const char *code);
/* Arm STAY and let the exit time run out. */
void sim_arm_stay_settled(sim_t *s, const char *code);

/* ---- queries ------------------------------------------------------------ */
alarm_partition_status_t sim_status(sim_t *s, uint8_t p);
alarm_zone_status_t sim_zstatus(sim_t *s, uint16_t zone);
unsigned sim_count_events(const sim_t *s, alarm_event_type_t type);
const alarm_event_t *sim_last_event(const sim_t *s, alarm_event_type_t type);
unsigned sim_count_reports(const sim_t *s, alarm_event_type_t type);
const alarm_event_t *sim_last_report(const sim_t *s, alarm_event_type_t type);
bool sim_alarm_reported(const sim_t *s, uint16_t zone, alarm_cause_t cause);
bool sim_alarm_logged(const sim_t *s, uint16_t zone, alarm_cause_t cause);
unsigned sim_count_sounds(const sim_t *s, alarm_sound_t sound);
unsigned sim_count_zone_sounds(const sim_t *s, alarm_sound_t sound, uint16_t zone);
/* Latest pattern written to an output (ALARM_PAT_OFF when never written). */
alarm_pattern_t sim_output(const sim_t *s, alarm_output_t output, uint8_t partition);

/* ---- readable assertions ------------------------------------------------ */
#define EXPECT_RESULT(expected, actual) \
    TEST_ASSERT_EQUAL_STRING(alarm_result_name(expected), alarm_result_name(actual))
#define EXPECT_STATE(s, p, st) \
    TEST_ASSERT_EQUAL_STRING(alarm_state_name(st), alarm_state_name(sim_status((s), (p)).state))
#define EXPECT_LEVEL(s, p, lv) \
    TEST_ASSERT_EQUAL_STRING(alarm_level_name(lv), alarm_level_name(sim_status((s), (p)).level))
#define EXPECT_SOUNDER(s, p, snd) \
    TEST_ASSERT_EQUAL_STRING(alarm_sound_name(snd), alarm_sound_name(sim_status((s), (p)).sounder))
#define EXPECT_BELL(s, p, pat) \
    TEST_ASSERT_EQUAL_INT((int)(pat), (int)sim_status((s), (p)).bell)
#define EXPECT_EVENTS(s, type, n) TEST_ASSERT_EQUAL_UINT_MESSAGE((n), sim_count_events((s), (type)), alarm_event_name(type))
#define EXPECT_REPORTS(s, type, n) TEST_ASSERT_EQUAL_UINT_MESSAGE((n), sim_count_reports((s), (type)), alarm_event_name(type))
#define EXPECT_ZONE_FLAG(s, z, flag) TEST_ASSERT_TRUE((sim_zstatus((s), (z)).flags & (flag)) != 0u)
#define EXPECT_NO_ZONE_FLAG(s, z, flag) TEST_ASSERT_FALSE((sim_zstatus((s), (z)).flags & (flag)) != 0u)

#endif /* ALARM_SIM_H */
