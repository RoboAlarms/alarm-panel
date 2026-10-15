/*
 * clock_keeper.c - which clock the panel believes (docs/features/35). See clock_keeper.h.
 */
#include "rtc_clock/clock_keeper.h"

#include <string.h>

static const clock_action_t NOTHING = {CLOCK_DO_NOTHING, 0, 0};

void clock_keeper_init(clock_keeper_t *k)
{
    memset(k, 0, sizeof(*k));
    k->module = CLOCK_MODULE_ABSENT;
    k->source = CLOCK_SRC_NONE;
}

bool clock_keeper_epoch_ok(int64_t epoch_s)
{
    return epoch_s >= CLOCK_EPOCH_MIN_S && epoch_s <= CLOCK_EPOCH_MAX_S;
}

/* The module's time, when it holds one worth believing. */
static bool module_time(const uint8_t regs[DS3231_REG_COUNT], int64_t *epoch_s)
{
    ds3231_status_t st;
    ds3231_parse_status(regs, &st);
    return !st.osc_stopped && ds3231_decode_time(regs, epoch_s) && clock_keeper_epoch_ok(*epoch_s);
}

static void note_temperature(clock_keeper_t *k, const uint8_t regs[DS3231_REG_COUNT])
{
    ds3231_status_t st;
    ds3231_parse_status(regs, &st);
    k->temp_c4 = st.temp_c4;
    k->temp_known = true;
}

static bool module_usable(const clock_keeper_t *k)
{
    return k->module == CLOCK_MODULE_OK || k->module == CLOCK_MODULE_LOST;
}

clock_action_t clock_keeper_start(clock_keeper_t *k, bool answered,
                                  const uint8_t regs[DS3231_REG_COUNT], uint64_t now_ms)
{
    if (!answered) {
        k->module = CLOCK_MODULE_ABSENT;
        return NOTHING;
    }
    if (!ds3231_identify(regs)) {
        k->module = CLOCK_MODULE_OTHER; /* never written: it is somebody else's chip */
        return NOTHING;
    }
    note_temperature(k, regs);
    k->next_check_ms = now_ms + CLOCK_CHECK_FIRST_MS;
    int64_t epoch_s;
    if (!module_time(regs, &epoch_s)) {
        k->module = CLOCK_MODULE_LOST;
        k->lost_at_start = true;
        return NOTHING;
    }
    k->module = CLOCK_MODULE_OK;
    k->source = CLOCK_SRC_MODULE;
    /* One read cannot tell where in the second the module was: take the middle (the first
     * comparison, a minute later, reads it on its tick). */
    const clock_action_t set = {CLOCK_DO_SET, epoch_s * 1000 + 500, 0};
    return set;
}

void clock_keeper_internet(clock_keeper_t *k, uint64_t now_ms)
{
    k->source = CLOCK_SRC_INTERNET;
    k->internet_seen = true;
    k->internet_at_ms = now_ms;
    if (module_usable(k)) {
        k->write_pending = true;
        k->next_write_ms = now_ms;
    }
}

bool clock_keeper_internet_trusted(const clock_keeper_t *k, uint64_t now_ms)
{
    return k->internet_seen && now_ms - k->internet_at_ms < (uint64_t)CLOCK_INTERNET_TRUST_MS;
}

bool clock_keeper_can_set(const clock_keeper_t *k)
{
    return module_usable(k);
}

clock_action_t clock_keeper_manual(clock_keeper_t *k, int64_t epoch_ms, uint64_t now_ms)
{
    if (!module_usable(k) || epoch_ms < 0 || !clock_keeper_epoch_ok(epoch_ms / 1000)) {
        return NOTHING;
    }
    k->source = CLOCK_SRC_MANUAL;
    k->write_pending = true;
    k->next_write_ms = now_ms;
    const clock_action_t set = {CLOCK_DO_SET, epoch_ms, 0};
    return set;
}

bool clock_keeper_write_due(const clock_keeper_t *k, uint64_t now_ms)
{
    return k->write_pending && k->source != CLOCK_SRC_NONE && module_usable(k) &&
           now_ms >= k->next_write_ms;
}

static void bus_failed(clock_keeper_t *k)
{
    if (k->failures < UINT8_MAX) {
        k->failures++;
    }
    if (k->failures >= CLOCK_FAILS_TO_LOSE && module_usable(k)) {
        k->module = CLOCK_MODULE_FAILED;
    }
}

void clock_keeper_written(clock_keeper_t *k, bool ok, uint64_t now_ms)
{
    if (!ok) {
        k->write_failures++;
        bus_failed(k);
        k->next_write_ms = now_ms + CLOCK_RETRY_MS;
        return;
    }
    k->writes++;
    k->write_pending = false;
    k->failures = 0;
    k->module = CLOCK_MODULE_OK; /* the write cleared the oscillator-stop flag */
    k->offset_known = false;
    k->next_check_ms = now_ms + CLOCK_CHECK_FIRST_MS; /* see that it took */
}

bool clock_keeper_check_due(const clock_keeper_t *k, uint64_t now_ms)
{
    return (module_usable(k) || k->module == CLOCK_MODULE_FAILED) && now_ms >= k->next_check_ms;
}

static int64_t magnitude(int64_t v)
{
    return v < 0 ? -v : v;
}

clock_action_t clock_keeper_compare(clock_keeper_t *k, bool answered,
                                    const uint8_t regs[DS3231_REG_COUNT], int64_t system_ms,
                                    uint64_t now_ms)
{
    if (!answered || !ds3231_identify(regs)) { /* a garbled read counts as a failed one */
        bus_failed(k);
        k->next_check_ms = now_ms + CLOCK_RETRY_MS;
        return NOTHING;
    }
    k->failures = 0;
    k->next_check_ms = now_ms + CLOCK_CHECK_EVERY_MS;
    note_temperature(k, regs);
    int64_t module_s;
    if (!module_time(regs, &module_s)) {
        /* It lost the time while the panel ran (a power glitch with a flat coin cell): give it
         * the panel's own, when the panel has one. */
        k->module = CLOCK_MODULE_LOST;
        k->offset_known = false;
        if (k->source != CLOCK_SRC_NONE) {
            k->write_pending = true;
        }
        return NOTHING;
    }
    k->module = CLOCK_MODULE_OK;
    if (k->source == CLOCK_SRC_NONE) {
        /* The panel has no time of its own yet, and the module does (it came back): take it.
         * Read on its tick, so the second is exact. */
        k->source = CLOCK_SRC_MODULE;
        k->offset_known = false;
        const clock_action_t set = {CLOCK_DO_SET, module_s * 1000, 0};
        return set;
    }
    const int64_t offset = module_s * 1000 - system_ms;
    k->last_offset_ms = offset;
    k->offset_known = true;
    if (clock_keeper_internet_trusted(k, now_ms)) {
        if (magnitude(offset) >= CLOCK_REWRITE_MS) {
            k->write_pending = true; /* the internet is the better clock */
            k->next_write_ms = now_ms;
        }
        return NOTHING;
    }
    if (magnitude(offset) < CLOCK_SLEW_MIN_MS) {
        return NOTHING;
    }
    if (magnitude(offset) <= CLOCK_SLEW_MAX_MS) {
        k->slews++;
        const clock_action_t slew = {CLOCK_DO_SLEW, 0, offset};
        return slew;
    }
    /* The panel's clock only drifts (tens of ms an hour); a module a minute or more away was
     * changed behind the panel's back or misread its own registers. Put it back. */
    k->jumps++;
    k->write_pending = true;
    k->next_write_ms = now_ms;
    return NOTHING;
}

const char *clock_module_name(clock_module_t m)
{
    switch (m) {
    case CLOCK_MODULE_ABSENT: return "not fitted";
    case CLOCK_MODULE_OK: return "keeping time";
    case CLOCK_MODULE_LOST: return "lost the time";
    case CLOCK_MODULE_FAILED: return "not answering";
    case CLOCK_MODULE_OTHER: return "not a DS3231";
    }
    return "?";
}

const char *clock_source_name(clock_source_t s)
{
    switch (s) {
    case CLOCK_SRC_NONE: return "not set";
    case CLOCK_SRC_MODULE: return "clock module";
    case CLOCK_SRC_INTERNET: return "time server";
    case CLOCK_SRC_MANUAL: return "set by hand";
    }
    return "?";
}
