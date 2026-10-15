/* Small Panel Manager decisions kept free of ESP-IDF (web_rules.h). */
#include "web_rules.h"

bool web_attach(cJSON *parent, const char *key, cJSON *child)
{
    bool ok = false;
    if (parent && child)
        ok = key ? cJSON_AddItemToObject(parent, key, child) : cJSON_AddItemToArray(parent, child);
    if (!ok)
        cJSON_Delete(child); /* never attached: nobody else would free it */
    return ok;
}

static bool in(uint8_t mine, unsigned partition)
{
    return partition >= 1u && partition <= ALARM_MAX_PARTITIONS &&
           (((unsigned)mine >> (partition - 1u)) & 1u) != 0u;
}

bool web_history_visible(const alarm_config_t *cfg, const alarm_event_t *ev, uint8_t mine)
{
    if (mine == 0xFFu)
        return true; /* the installer and the system master see everything */
    if (ev->partition != 0u)
        return in(mine, ev->partition);
    /* The system's own. A device trouble's "zone" is the device's number, not a zone. */
    const bool device = (ev->type == ALARM_EV_TROUBLE || ev->type == ALARM_EV_TROUBLE_RESTORE) &&
                        ev->arg2 == ALARM_TRBL_FAMILY_DEVICE;
    if (ev->zone == 0u || device)
        return true;
    return ev->zone <= ALARM_MAX_ZONES && in(mine, cfg->zones[ev->zone - 1u].partition);
}

bool web_history_keep(void *ctx, const elog_entry_t *entry)
{
    const web_history_view_t *v = ctx;
    return entry->kind != (uint8_t)ELOG_KIND_EVENT || web_history_visible(v->cfg, &entry->ev, v->mine);
}

bool web_send_all(const web_send_io_t *io, const uint8_t *p, size_t n, int64_t give_up_us)
{
    while (n > 0u) {
        if (io->stopping(io->ctx) || io->now_us(io->ctx) > give_up_us)
            return false;
        /* The same piece again after a try that sent nothing (TLS asks for the same buffer). */
        const size_t piece = io->piece && io->piece < n ? io->piece : n;
        const int w = io->write(io->ctx, p, piece);
        if (w < 0 || (size_t)w > piece)
            return false;
        if (w == 0) {
            io->pause(io->ctx);
            continue;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}
