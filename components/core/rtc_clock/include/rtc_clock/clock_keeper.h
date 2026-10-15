#pragma once
/*
 * clock_keeper.h - which clock the panel believes, and when the battery-backed clock module is
 * read, written or followed (docs/features/35 RTC-003..RTC-009).
 *
 * The rules, in short:
 *   - At start-up a module that kept a valid time sets the panel's clock before anything is
 *     logged. One that lost it (oscillator-stop flag, an impossible date, before 2025) is not
 *     believed: the clock stays "not set" until the internet or a person sets it.
 *   - The time server is the best clock there is: every sync writes the module, and while it is
 *     trusted (CLOCK_INTERNET_TRUST_MS after a sync) a module that is a second or more off is
 *     written again.
 *   - Without the internet the module (+-2 ppm) is the better clock: every hour the panel reads
 *     it on its second tick and slews its own clock (adjtime) when they differ by
 *     CLOCK_SLEW_MIN_MS..CLOCK_SLEW_MAX_MS. A bigger difference means the module jumped (the
 *     panel's own clock only drifts), so the module is put back instead.
 *   - A person may set the time only while a module answers: without one it would be gone at the
 *     next power cut.
 *
 * The keeper decides; the service reads the chip, sets the system clock and writes the chip.
 * Times: now_ms is monotonic (since start-up), epoch values are UTC. Portable C11.
 */
#include <stdbool.h>
#include <stdint.h>

#include "rtc_clock/ds3231.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_EPOCH_MIN_S 1735689600LL    /* 2025-01-01: an earlier clock was never set */
#define CLOCK_EPOCH_MAX_S 4102444799LL    /* 2099-12-31 23:59:59 */
#define CLOCK_INTERNET_TRUST_MS 86400000u /* a day: the time server syncs every 6 h */
#define CLOCK_CHECK_FIRST_MS 60000u       /* the first comparison after start-up or a write */
#define CLOCK_CHECK_EVERY_MS 3600000u
#define CLOCK_RETRY_MS 60000u             /* after a bus failure, and a failed write */
#define CLOCK_FAILS_TO_LOSE 3u            /* failures in a row before "not answering" */
#define CLOCK_SLEW_MIN_MS 250             /* closer than this is left alone */
#define CLOCK_SLEW_MAX_MS 60000           /* further than this: the module jumped */
#define CLOCK_REWRITE_MS 1000             /* a module this far from internet time is rewritten */

typedef enum {
    CLOCK_MODULE_ABSENT = 0, /* nothing answers at 0x68 (most panels) */
    CLOCK_MODULE_OK,         /* answering and holding the time */
    CLOCK_MODULE_LOST,       /* answering, but without a time to believe until it is written */
    CLOCK_MODULE_FAILED,     /* was answering, and stopped */
    CLOCK_MODULE_OTHER,      /* something that isn't a DS3231 answers at its address */
} clock_module_t;

/* What set the panel's clock last. */
typedef enum {
    CLOCK_SRC_NONE = 0, /* nothing since power-up */
    CLOCK_SRC_MODULE,   /* the clock module */
    CLOCK_SRC_INTERNET, /* the time server */
    CLOCK_SRC_MANUAL,   /* a person, at the panel or in the browser */
} clock_source_t;

typedef enum {
    CLOCK_DO_NOTHING = 0,
    CLOCK_DO_SET,  /* step the system clock to epoch_ms */
    CLOCK_DO_SLEW, /* slew the system clock by delta_ms (adjtime) */
} clock_do_t;

typedef struct {
    clock_do_t kind;
    int64_t epoch_ms; /* CLOCK_DO_SET */
    int64_t delta_ms; /* CLOCK_DO_SLEW: positive = the system clock is behind */
} clock_action_t;

typedef struct {
    clock_module_t module;
    clock_source_t source;
    bool lost_at_start;    /* the module had no time when the panel started: coin cell flat? */
    bool write_pending;    /* write the module from the system clock at its next whole second */
    uint8_t failures;      /* bus failures in a row */
    bool internet_seen;
    uint64_t internet_at_ms;
    uint64_t next_check_ms;
    uint64_t next_write_ms;
    bool offset_known;
    int64_t last_offset_ms; /* module minus system at the last comparison */
    bool temp_known;
    int16_t temp_c4;        /* the module's temperature, quarter degrees */
    uint32_t writes, write_failures, slews, jumps;
} clock_keeper_t;

void clock_keeper_init(clock_keeper_t *k);

/* A moment the clock can hold (2025..2099). */
bool clock_keeper_epoch_ok(int64_t epoch_s);

/* Start-up: answered false = nothing acknowledged at 0x68; regs the 19 registers read. Returns
 * CLOCK_DO_SET with the module's time (the middle of the second it showed) when it kept one. */
clock_action_t clock_keeper_start(clock_keeper_t *k, bool answered,
                                  const uint8_t regs[DS3231_REG_COUNT], uint64_t now_ms);

/* The time server has just set the system clock. */
void clock_keeper_internet(clock_keeper_t *k, uint64_t now_ms);
bool clock_keeper_internet_trusted(const clock_keeper_t *k, uint64_t now_ms);

/* Whether a person may set the time: a module answers and can keep it. */
bool clock_keeper_can_set(const clock_keeper_t *k);
/* A person set the time: CLOCK_DO_SET, and the module is written next. CLOCK_DO_NOTHING when
 * refused (no module, or a moment outside 2025..2099). */
clock_action_t clock_keeper_manual(clock_keeper_t *k, int64_t epoch_ms, uint64_t now_ms);

/* Whether the module should be written now, and what came of the write. */
bool clock_keeper_write_due(const clock_keeper_t *k, uint64_t now_ms);
void clock_keeper_written(clock_keeper_t *k, bool ok, uint64_t now_ms);

/* Whether it is time to compare the module with the system clock. */
bool clock_keeper_check_due(const clock_keeper_t *k, uint64_t now_ms);
/* A comparison. answered false = the bus failed. regs were read right after the module's
 * seconds ticked over, and system_ms is the system clock at that tick. */
clock_action_t clock_keeper_compare(clock_keeper_t *k, bool answered,
                                    const uint8_t regs[DS3231_REG_COUNT], int64_t system_ms,
                                    uint64_t now_ms);

/* For the console and the log. */
const char *clock_module_name(clock_module_t m);
const char *clock_source_name(clock_source_t s);

#ifdef __cplusplus
}
#endif
