#pragma once
/*
 * panel_clock.h - the battery-backed clock module (docs/features/35).
 *
 * Most panels have none, and for them nothing here changes anything. With a DS3231 on the
 * board's I2C connector the panel knows the time from the moment it starts - before the
 * internet, and without it - and a person may set the time by hand. The rules are
 * components/core/rtc_clock's clock_keeper; this service does the bus work and the system
 * clock (settimeofday / adjtime), on its own task.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "rtc_clock/clock_keeper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool built;              /* false: the firmware has no clock-module support */
    clock_module_t module;
    clock_source_t source;   /* what set the panel's clock last */
    bool internet_trusted;   /* a time-server sync within the last day */
    bool lost_at_start;      /* the module had no time at start-up: its coin cell may be flat */
    bool temp_known;
    int16_t temp_c4;         /* the module's temperature, quarter degrees C */
    bool offset_known;
    int64_t offset_ms;       /* module minus panel at the last comparison */
    uint32_t writes, write_failures, slews, jumps;
    const char *where;       /* where the module plugs in ("I2C connector (PH2.0, 4 pins)") */
} panel_clock_status_t;

/* Where the module may be wired (`rtc place`, RTC-013): the app knows the board, the service
 * only asks. Given before panel_clock_start, even when the board has no connector. */
typedef struct {
    const char *own_where; /* the board's own add-on connector, "" when it has none */
    /* The board's other places, one line each for `rtc place`: "j7: J7 header, IO27 (SDA) and
     * IO28 (SCL)"; "" or NULL when it has none. */
    const char *places;
    /* May the module move to the place called id? false with the reason in why. */
    bool (*place_ok)(const char *id, char *why, size_t len);
    /* May the module go on these two pins of the person's own (`rtc place custom`, RTC-016)?
     * NULL: refused. */
    bool (*custom_ok)(int16_t sda, int16_t scl, char *why, size_t len);
    /* Why a stored custom place was not used this start ("custom place not used: ..."), for
     * `rtc` and `rtc place`; NULL when it was, or none is stored. */
    const char *custom_why;
} panel_clock_place_rules_t;

typedef enum {
    PANEL_CLOCK_CUSTOM_NONE = 0, /* the stored place is not "custom" */
    PANEL_CLOCK_CUSTOM_STORED,   /* it is, and its pins read back */
    PANEL_CLOCK_CUSTOM_DAMAGED,  /* it is, but a pin is missing or out of range */
} panel_clock_custom_t;

void panel_clock_place_rules(const panel_clock_place_rules_t *rules);

/* The place chosen with `rtc place` (NVS "clock"/"place"), copied into out: false for the
 * board's own connector. The app reads it before start-up to open the right bus. */
bool panel_clock_stored_place(char *out, size_t len);
/* The pins stored with `rtc place custom` (NVS clock/c_sda, clock/c_scl; RTC-016), set when
 * PANEL_CLOCK_CUSTOM_STORED. */
panel_clock_custom_t panel_clock_stored_custom(int16_t *sda, int16_t *scl);

/* Start-up, before the event log and the engine: looks for the module on bus (NULL: the board
 * has no connector), sets the panel's clock from it when it kept the time, and starts the
 * task. place is the board place the bus is at (NULL: its own connector). Returns ESP_OK with
 * or without a module; an error only when the task can't start. */
esp_err_t panel_clock_start(i2c_master_bus_handle_t bus, const char *where, const char *place);

void panel_clock_get(panel_clock_status_t *out);

/* A person sets the date and time (UTC ms); who is for the log ("the panel", "the browser").
 * ESP_ERR_NOT_SUPPORTED without a module that answers (the time would be gone at the next
 * power cut), ESP_ERR_INVALID_ARG outside 2025..2099. The panel's clock changes at once, the
 * module at its next whole second. */
esp_err_t panel_clock_set(int64_t epoch_ms, const char *who);

/* `rtc` at the service console. */
esp_err_t panel_clock_console_register(void);

#ifdef __cplusplus
}
#endif
