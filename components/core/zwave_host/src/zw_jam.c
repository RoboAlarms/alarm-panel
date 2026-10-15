/*
 * zw_jam.c - is the radio jammed? (see zw_jam.h).
 */
#include "zwave_host/zw_jam.h"

#include <string.h>

#include "zwave_proto/zw_api.h"

void zw_jam_init(zw_jam_t *j, int8_t threshold_dbm)
{
    memset(j, 0, sizeof *j);
    j->threshold = threshold_dbm;
}

void zw_jam_set_threshold(zw_jam_t *j, int8_t threshold_dbm)
{
    j->threshold = threshold_dbm;
    j->loud = false; /* judged again from the next reading */
}

void zw_jam_reset(zw_jam_t *j)
{
    zw_jam_init(j, j->threshold);
}

bool zw_jam_jammed(const zw_jam_t *j)
{
    return j->jammed;
}

/* The failures of the last ZW_JAM_HOLD_MS only. */
static void age_fails(zw_jam_t *j, uint64_t now)
{
    size_t keep = 0u;
    for (size_t i = 0; i < j->fail_n; i++) {
        if (now - j->fails[i] < ZW_JAM_HOLD_MS) {
            j->fails[keep++] = j->fails[i];
        }
    }
    j->fail_n = keep;
}

/* Jammed or not, from what is known now; true when it changed. */
static bool judge(zw_jam_t *j, uint64_t now)
{
    age_fails(j, now);
    const bool by_noise = j->loud && now - j->loud_since >= ZW_JAM_HOLD_MS;
    const bool by_fails = j->fail_n >= ZW_JAM_TX_FAILS;
    bool jammed = j->jammed;
    if (by_noise || by_fails) {
        jammed = true;
    } else if (j->jammed) {
        /* clears once the noise has been below the threshold for the hold time (or was never the
         * reason) and no burst of failures is recent */
        const bool quiet = !j->loud && (j->quiet_since == 0u || now - j->quiet_since >= ZW_JAM_HOLD_MS);
        jammed = !(quiet && j->fail_n == 0u);
    }
    const bool changed = jammed != j->jammed;
    j->jammed = jammed;
    return changed;
}

bool zw_jam_reading(zw_jam_t *j, uint64_t now, const uint8_t *rssi, size_t n)
{
    bool any = false;
    int8_t quietest = 127;
    j->channels = 0u;
    for (size_t i = 0; rssi != NULL && i < n && i < ZW_JAM_CHANNELS; i++) {
        const int8_t v = (int8_t)rssi[i];
        j->rssi[i] = v;
        j->channels++;
        int8_t level;
        if (v == ZW_RSSI_NOT_AVAILABLE) {
            continue;
        } else if (v == ZW_RSSI_SATURATED) {
            level = 126; /* louder than anything */
        } else if (v == ZW_RSSI_NO_SIGNAL) {
            level = -128; /* below the radio's sensitivity: quiet */
        } else {
            level = v;
        }
        any = true;
        if (level < quietest) {
            quietest = level;
        }
    }
    if (!any) {
        zw_jam_no_reading(j, now);
        return judge(j, now);
    }
    j->has_reading = true;
    j->quietest = quietest;
    const bool loud = quietest >= j->threshold;
    if (loud && !j->loud) {
        j->loud_since = now;
    }
    if (!loud && (j->loud || j->quiet_since == 0u)) {
        j->quiet_since = now;
    }
    j->loud = loud;
    return judge(j, now);
}

void zw_jam_no_reading(zw_jam_t *j, uint64_t now)
{
    (void)now;
    j->has_reading = false;
    if (!j->jammed) {
        j->loud = false; /* a stale sample never proves a jam: the wait starts again */
    } /* ...nor ends one: while jammed, a missing reading proves nothing either way */
}

bool zw_jam_tx_failed(zw_jam_t *j, uint64_t now)
{
    age_fails(j, now);
    if (j->fail_n == ZW_JAM_TX_FAILS) {
        memmove(&j->fails[0], &j->fails[1], (ZW_JAM_TX_FAILS - 1u) * sizeof j->fails[0]);
        j->fail_n--;
    }
    j->fails[j->fail_n++] = now;
    return judge(j, now);
}

bool zw_jam_poll(zw_jam_t *j, uint64_t now)
{
    return judge(j, now);
}
