#include "power_monitor.h"
#include <string.h>

bool power_source_none(void *ctx, power_sample_t *out)
{
    (void)ctx;
    *out = (power_sample_t){.battery_mv = -1, .battery_percent = -1};
    return true;
}

void power_monitor_init(power_monitor_t *m)
{
    memset(m, 0, sizeof(*m));
    (void)power_source_none(NULL, &m->status);
}

static power_value_t filter(power_filter_t *f, power_value_t value, uint64_t now)
{
    if (value != POWER_NO && value != POWER_YES) {
        memset(f, 0, sizeof(*f));
        return POWER_UNKNOWN;
    }
    if (value != f->candidate) {
        f->candidate = value;
        f->since = now;
    }
    if (now - f->since >= 5000u) {
        f->value = value;
    }
    return f->value;
}

void power_monitor_poll(power_monitor_t *m, const power_source_t *source, uint64_t now)
{
    power_sample_t sample;
    (void)power_source_none(NULL, &sample);
    if (source != NULL && source->read != NULL && !source->read(source->ctx, &sample)) {
        (void)power_source_none(NULL, &sample);
    }
    if (m->sampled && (now < m->last_poll || now - m->last_poll > 15000u)) {
        power_monitor_init(m);
    }
    m->sampled = true;
    m->last_poll = now;
    m->status.ac_present = filter(&m->ac, sample.ac_present, now);
    m->status.battery_present = filter(&m->present, sample.battery_present, now);
    m->status.charging = filter(&m->charging, sample.charging, now);
    /* Never infer a healthy/low battery from an absent or unreadable one. */
    const bool battery = sample.battery_present == POWER_YES &&
                         m->status.battery_present == POWER_YES;
    /* Voltage hysteresis for the native one-cell Li-ion source. In the deadband,
     * keep an established low state; do not invent a restore on a failed read. */
    if (battery && sample.battery_low == POWER_UNKNOWN && sample.battery_mv > 3500 &&
        sample.battery_mv < 3700) {
        sample.battery_low = m->low.value;
    }
    m->status.battery_low = filter(&m->low, battery ? sample.battery_low : POWER_UNKNOWN, now);
    m->status.battery_mv = battery && sample.battery_mv >= 0 ? sample.battery_mv : -1;
    m->status.battery_percent = battery && sample.battery_percent >= 0 &&
                              sample.battery_percent <= 100 ? sample.battery_percent : -1;
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool power_decode_stc(const uint8_t *bytes, size_t len, power_sample_t *out)
{
    (void)power_source_none(NULL, out);
    if (bytes == NULL || len != 11u) {
        return false;
    }
    const uint32_t adc = read_le32(bytes);
    const uint32_t mv = read_le32(bytes + 4);
    if (adc == 0u || adc > 3400u || mv == 0u || mv > 6000u ||
        bytes[8] > 100u || bytes[9] > 4u || bytes[10] > 4u) {
        return false;
    }
    /* V1.1 schematic R164=39k, R186=100k. Also rejects torn/mismapped fields. */
    const uint32_t expected = adc * 139u / 100u;
    if (mv + 100u < expected || mv > expected + 100u) {
        return false;
    }
    /* Charging/full confirms external input, but NO_CHARGE does not prove its
     * absence. The protocol has no independent AC-present/presence bit. */
    if (bytes[9] == 1u || bytes[9] == 2u) {
        out->ac_present = POWER_YES;
    }
    if (mv < 2000u || mv > 4350u || bytes[9] == 0u || bytes[9] == 4u) {
        return true; /* empty/invalid battery: critical UI, no false 100% */
    }
    out->battery_present = POWER_YES; /* inferred from plausible live cell voltage */
    out->battery_mv = (int)mv;
    out->battery_percent = bytes[8]; /* helper estimate, not a coulomb counter */
    out->charging = bytes[9] == 1u ? POWER_YES : POWER_NO;
    out->battery_low = mv <= 3500u ? POWER_YES : mv >= 3700u ? POWER_NO : POWER_UNKNOWN;
    return true;
}

uint8_t power_backlight_limit(uint8_t requested, bool saving)
{
    if (requested > 100u) {
        requested = 100u;
    }
    return saving && requested > 20u ? 20u : requested;
}

bool power_saving_update(power_saving_t *state, const power_sample_t *sample, uint64_t now)
{
    const bool want = sample->ac_present == POWER_YES ? false :
                      sample->battery_present == POWER_YES ? true : state->saving;
    if (want != state->candidate || now < state->since) {
        state->candidate = want;
        state->since = now;
    }
    if (now - state->since >= 15000u) {
        state->saving = want;
    }
    return state->saving;
}
