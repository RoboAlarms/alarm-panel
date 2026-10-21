/*
 * zwz_store.c - zwz_panel's dirty values into NVS "zwave" (zwz_store.h; network.md §2, ZW-017,
 * ZW-039).
 */
#include "zwave_zones/zwz_store.h"

#include <string.h>

#include "zwave_host/zw_crypto.h"
#include "zwave_zones/zw_outputs.h"

void zwz_store_init(zwz_store_t *s)
{
    memset(s, 0, sizeof *s);
}

static bool kept(const zwz_store_port_t *port)
{
    return port->commit == NULL || port->commit(port->ctx);
}

static bool store_blob(const zwz_store_port_t *port, const char *key, const uint8_t *data,
                       size_t len)
{
    return port->set_blob(port->ctx, key, data, len) && kept(port);
}

static bool store_u16(const zwz_store_port_t *port, const char *key, uint16_t v)
{
    return port->set_u16(port->ctx, key, v) && kept(port);
}

static void lock(const zwz_store_port_t *port)
{
    if (port->lock != NULL) {
        port->lock(port->ctx);
    }
}

static void unlock(const zwz_store_port_t *port)
{
    if (port->unlock != NULL) {
        port->unlock(port->ctx);
    }
}

uint8_t zwz_store_run(zwz_store_t *s, zwz_panel_t *p, uint64_t now, const zwz_store_port_t *port)
{
    uint8_t keys[ZWZ_KEYS_BLOB];
    uint8_t cfg[ZWZ_CFG_BLOB];
    uint8_t chk[ZWZ_CHECKINS_BLOB];
    uint8_t garage[ZWO_GARAGE_BLOB];
    uint8_t siren[ZWS_BLOB_MAX];
    size_t nodes_len = 0u;
    size_t keys_len = 0u;
    size_t cfg_len = 0u;
    size_t chk_len = 0u;
    size_t garage_len = 0u;
    size_t siren_len = 0u;
    size_t prov_len = 0u;
    uint16_t gen = 0u;
    if (s->retry_at != 0u && now < s->retry_at) {
        return 0u;
    }
    lock(port);
    const uint8_t dirty = zwz_panel_dirty(p);
    if (dirty != 0u) {
        if (port->nodes_buf != NULL) {
            nodes_len = zwz_panel_take_nodes(p, port->nodes_buf, port->nodes_cap, &gen);
        }
        keys_len = zwz_panel_take_keys(p, keys, sizeof keys);
        cfg_len = zwz_panel_take_cfg(p, cfg, sizeof cfg);
        chk_len = zwz_panel_take_checkins(p, chk, sizeof chk);
        garage_len = zwz_panel_take_garage(p, garage, sizeof garage);
        siren_len = zwz_panel_take_siren(p, siren, sizeof siren);
        if (port->prov_buf != NULL) {
            prov_len = zwz_panel_take_prov(p, port->prov_buf, port->prov_cap);
        }
    }
    unlock(port);
    if (dirty == 0u) {
        return 0u;
    }
    uint8_t failed = 0u;
    if (keys_len > 0u && !store_blob(port, ZWZ_STORE_KEY_KEYS, keys, keys_len)) {
        failed |= ZWZ_DIRTY_KEYS;
    }
    zw_wipe(keys, sizeof keys); /* never anywhere else */
    if (nodes_len > 0u && (!store_u16(port, ZWZ_STORE_KEY_GEN, gen) ||
                           !store_blob(port, ZWZ_STORE_KEY_NODES, port->nodes_buf, nodes_len))) {
        failed |= ZWZ_DIRTY_NODES;
    }
    if (cfg_len > 0u && !store_blob(port, ZWZ_STORE_KEY_CFG, cfg, cfg_len)) {
        failed |= ZWZ_DIRTY_CFG;
    }
    if (chk_len > 0u && !store_blob(port, ZWZ_STORE_KEY_CHK, chk, chk_len)) {
        failed |= ZWZ_DIRTY_CHECKINS;
    }
    if (garage_len > 0u && !store_blob(port, ZWZ_STORE_KEY_GARAGE, garage, garage_len)) {
        failed |= ZWZ_DIRTY_GARAGE;
    }
    if (prov_len > 0u && !store_blob(port, ZWZ_STORE_KEY_PROV, port->prov_buf, prov_len)) {
        failed |= ZWZ_DIRTY_PROV;
    }
    if (siren_len > 0u && !store_blob(port, ZWZ_STORE_KEY_SIREN, siren, siren_len)) {
        failed |= ZWZ_DIRTY_SIREN;
    }
    /* a value that did not fit its buffer stays dirty in zwz_panel: count it as refused */
    const unsigned taken = (nodes_len > 0u ? ZWZ_DIRTY_NODES : 0u) |
                           (keys_len > 0u ? ZWZ_DIRTY_KEYS : 0u) |
                           (cfg_len > 0u ? ZWZ_DIRTY_CFG : 0u) |
                           (chk_len > 0u ? ZWZ_DIRTY_CHECKINS : 0u) |
                           (garage_len > 0u ? ZWZ_DIRTY_GARAGE : 0u) |
                           (prov_len > 0u ? ZWZ_DIRTY_PROV : 0u) |
                           (siren_len > 0u ? ZWZ_DIRTY_SIREN : 0u);
    failed |= (uint8_t)((unsigned)dirty & ~taken);
    /* ZW-014: what is in storage now - the node cache's ends zwz_panel_identity_held() */
    const uint8_t stored = (uint8_t)(taken & ~(unsigned)failed);
    const bool nodes_stored = (stored & ZWZ_DIRTY_NODES) != 0u; /* the only bit it acts on */
    if (failed != 0u || nodes_stored) {
        lock(port);
        if (failed != 0u) {
            zwz_panel_redo(p, failed);
        }
        if (nodes_stored) {
            zwz_panel_stored(p, stored);
        }
        unlock(port);
    }
    s->retry_at = failed != 0u ? now + ZWZ_STORE_RETRY_MS : 0u;
    if (port->trouble != NULL) {
        port->trouble(port->ctx, failed != 0u);
    }
    return failed;
}

size_t zwz_store_read_max(size_t today)
{
    const size_t max = today <= (SIZE_MAX - 64u) / 2u ? 2u * today + 64u : SIZE_MAX;
    return max < ZWZ_STORE_READ_FLOOR ? ZWZ_STORE_READ_FLOOR : max;
}

zwz_store_read_t zwz_store_read_verdict(bool found, bool readable, size_t stored, size_t max)
{
    if (!found || (readable && stored == 0u)) {
        return ZWZ_STORE_READ_ABSENT;
    }
    if (!readable || stored > max) {
        return ZWZ_STORE_READ_REFUSED;
    }
    return ZWZ_STORE_READ_OK;
}
