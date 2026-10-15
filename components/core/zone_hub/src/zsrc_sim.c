/*
 * zsrc_sim.c - the simulator source (see zsrc_sim.h).
 */
#include "zone_hub/zsrc_sim.h"

#include <stdio.h>
#include <string.h>

static bool sim_valid(void *ctx, const zsrc_address_t *a)
{
    (void)ctx;
    return a->kind == ZSRC_SIM && a->input == 0u && a->id >= 1u && a->id <= ALARM_MAX_ZONES &&
           a->ref[0] == '\0';
}

static bool sim_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    (void)m;
    (void)now;
    return sim_valid(ctx, a); /* nothing to listen to: the console speaks for the sensor */
}

static void sim_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    (void)ctx;
    snprintf(out, len, "console input %u", (unsigned)a->id);
}

static const zone_source_ops_t SIM_OPS = {
    .kind = ZSRC_SIM,
    .caps = ZSRC_CAP_TEST | ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_LIFE_SAFETY | ZSRC_CAP_SUPERVISED |
            ZSRC_CAP_TAMPER | ZSRC_CAP_BATTERY,
    .name = "Simulator",
    .valid = sim_valid,
    .watch = sim_watch,
    .describe = sim_describe,
};

void zsrc_sim_init(zsrc_sim_t *s, const zsrc_sink_t *sink)
{
    memset(s, 0, sizeof(*s));
    s->sink = *sink;
}

const zone_source_ops_t *zsrc_sim_ops(void)
{
    return &SIM_OPS;
}

zsrc_address_t zsrc_sim_address(uint16_t input)
{
    return zsrc_address(ZSRC_SIM, 0u, input);
}

void zsrc_sim_event(zsrc_sim_t *s, uint16_t input, alarm_zone_event_t ev, uint64_t now)
{
    const zsrc_address_t a = zsrc_sim_address(input);
    if (s->sink.event != NULL) {
        s->sink.event(s->sink.ctx, &a, ev, now);
    }
}
