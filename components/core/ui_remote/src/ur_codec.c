/*
 * ur_codec.c - ui_remote's records (ur_internal.h) and the tables of the structs the calls carry.
 */
#include <string.h>

#include "ur_internal.h"

/* ---- strings ---- */

void ur_put_str(plink_w_t *w, const char *s)
{
    const size_t n = s != NULL ? strlen(s) : 0u;
    plink_w_u16(w, (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n));
    plink_w_bytes(w, s, n > 0xFFFFu ? 0xFFFFu : n);
}

void ur_get_str(plink_r_t *r, char *out, size_t cap, bool clean)
{
    if (cap == 0u) {
        return;
    }
    memset(out, 0, cap);
    size_t n = plink_r_u16(r);
    const size_t left = plink_r_left(r);
    if (n > left) {
        if (!r->pad) {
            r->ok = false;
            return;
        }
        n = left;
    }
    const uint8_t *s = plink_r_take(r, n);
    if (s == NULL) {
        return;
    }
    if (clean) {
        plink_clean_text((const char *)s, n, out, cap);
        return;
    }
    /* a code or a password: as typed, cut to the field (never on a NUL inside it) */
    const size_t k = n < cap - 1u ? n : cap - 1u;
    memcpy(out, s, k);
    out[strnlen(out, k)] = '\0';
}

/* ---- records ---- */

static void put_int(plink_w_t *w, const uint8_t *p, size_t size)
{
    uint64_t v = 0;
    switch (size) {
    case 1: v = *p; break;
    case 2: { uint16_t x; memcpy(&x, p, 2); v = x; break; }
    case 4: { uint32_t x; memcpy(&x, p, 4); v = x; break; }
    case 8: memcpy(&v, p, 8); break;
    default: w->ok = false; return;
    }
    for (size_t i = 0; i < size; i++) {
        plink_w_u8(w, (uint8_t)(v >> (8u * i)));
    }
}

static void get_int(plink_r_t *r, uint8_t *p, size_t size)
{
    uint64_t v = 0;
    for (size_t i = 0; i < size; i++) {
        v |= (uint64_t)plink_r_u8(r) << (8u * i);
    }
    switch (size) {
    case 1: *p = (uint8_t)v; break;
    case 2: { const uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { const uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    case 8: memcpy(p, &v, 8); break;
    default: r->ok = false; break;
    }
}

void ur_put(plink_w_t *w, const ur_table_t *t, const void *obj)
{
    const size_t at = w->len;
    plink_w_u16(w, 0); /* the record's length, set below */
    const uint8_t *base = obj;
    for (size_t i = 0; i < t->n; i++) {
        const ur_field_t *f = &t->f[i];
        for (size_t e = 0; e < f->count; e++) {
            const uint8_t *p = base + f->off + e * f->size;
            switch (f->kind) {
            case UR_F_INT: put_int(w, p, f->size); break;
            case UR_F_BOOL: plink_w_u8(w, *(const bool *)(const void *)p ? 1u : 0u); break;
            case UR_F_STR:
            case UR_F_SECRET: {
                const size_t n = strnlen((const char *)p, f->size);
                plink_w_u16(w, (uint16_t)n);
                plink_w_bytes(w, p, n);
                break;
            }
            case UR_F_REC: ur_put(w, f->sub, p); break;
            default: w->ok = false; break;
            }
        }
    }
    const size_t n = w->len - at - 2u;
    if (!w->ok || n > 0xFFFFu) {
        w->ok = false;
        return;
    }
    w->p[at] = (uint8_t)n;
    w->p[at + 1u] = (uint8_t)(n >> 8);
}

void ur_get(plink_r_t *r, const ur_table_t *t, void *obj)
{
    memset(obj, 0, t->size);
    const size_t n = plink_r_u16(r);
    const uint8_t *at = plink_r_take(r, n);
    if (at == NULL) {
        r->ok = false;
        return;
    }
    /* the record's own reader: a field it lacks (a sender older than this table) reads zero, and
     * fields it has past this table (a newer sender) are skipped with the rest of the record */
    plink_r_t rr;
    plink_r_init(&rr, at, n);
    rr.pad = true;
    uint8_t *base = obj;
    for (size_t i = 0; i < t->n && rr.ok; i++) {
        const ur_field_t *f = &t->f[i];
        for (size_t e = 0; e < f->count; e++) {
            uint8_t *p = base + f->off + e * f->size;
            switch (f->kind) {
            case UR_F_INT: get_int(&rr, p, f->size); break;
            case UR_F_BOOL: *(bool *)(void *)p = plink_r_u8(&rr) != 0u; break;
            case UR_F_STR: ur_get_str(&rr, (char *)p, f->size, true); break;
            case UR_F_SECRET: ur_get_str(&rr, (char *)p, f->size, false); break;
            case UR_F_REC:
                if (plink_r_left(&rr) > 0u) {
                    ur_get(&rr, f->sub, p);
                }
                break;
            default: rr.ok = false; break;
            }
        }
    }
    if (!rr.ok) {
        r->ok = false;
    }
}

void ur_put_list(plink_w_t *w, const ur_table_t *t, const void *objs, size_t count)
{
    plink_w_u16(w, (uint16_t)count);
    for (size_t i = 0; i < count; i++) {
        ur_put(w, t, (const uint8_t *)objs + i * t->size);
    }
}

size_t ur_get_list(plink_r_t *r, const ur_table_t *t, void *objs, size_t max)
{
    const size_t count = plink_r_u16(r);
    size_t got = 0;
    for (size_t i = 0; i < count && r->ok; i++) {
        if (got < max) {
            ur_get(r, t, (uint8_t *)objs + got * t->size);
            got++;
        } else { /* more than the caller holds: skip the rest */
            const size_t n = plink_r_u16(r);
            (void)plink_r_take(r, n);
        }
    }
    return r->ok ? got : 0u;
}

/* ---- the tables (append only: the frozen fixture holds each one's bytes) ---- */

static const ur_field_t f_address[] = {
    UR_INT(zsrc_address_t, kind),
    UR_INT(zsrc_address_t, input),
    UR_INT(zsrc_address_t, id),
    UR_STR(zsrc_address_t, ref),
};
static const ur_table_t t_address = UR_TABLE(zsrc_address_t, f_address);

static const ur_field_t f_mapping[] = {
    UR_INT(zsrc_mapping_t, mode),
    UR_BOOL(zsrc_mapping_t, invert),
    UR_INT(zsrc_mapping_t, threshold_x100),
    UR_INT(zsrc_mapping_t, hysteresis_x100),
    UR_STR(zsrc_mapping_t, states),
    UR_INT(zsrc_mapping_t, grace_s),
    UR_INT(zsrc_mapping_t, response_ms),
    UR_INT(zsrc_mapping_t, hold_s),
};
static const ur_table_t t_mapping = UR_TABLE(zsrc_mapping_t, f_mapping);

static const ur_field_t f_input[] = {
    UR_REC(zsrc_input_t, addr, t_address),
    UR_REC(zsrc_input_t, map, t_mapping),
};
static const ur_table_t t_input = UR_TABLE(zsrc_input_t, f_input);

static const ur_field_t f_binding[] = {
    UR_RECS(zone_hub_binding_t, in, t_input),
};
const ur_table_t ur_t_binding = UR_TABLE(zone_hub_binding_t, f_binding);

static const ur_field_t f_info[] = {
    UR_REC(zone_hub_info_t, binding, ur_t_binding),
    UR_BOOL(zone_hub_info_t, lost),
    UR_BOOL(zone_hub_info_t, source_present),
    UR_INT(zone_hub_info_t, caps),
    UR_BOOL(zone_hub_info_t, link_down),
    UR_BOOL(zone_hub_info_t, unreachable),
    UR_BOOL(zone_hub_info_t, check),
    UR_BOOL(zone_hub_info_t, known),
    UR_BOOL(zone_hub_info_t, active),
    UR_BOOL(zone_hub_info_t, has_value),
    UR_INT(zone_hub_info_t, value_x100),
    UR_INT(zone_hub_info_t, heard_at),
    UR_STR(zone_hub_info_t, address),
};
const ur_table_t ur_t_info = UR_TABLE(zone_hub_info_t, f_info);

static const ur_field_t f_candidate[] = {
    UR_REC(zsrc_candidate_t, addr, t_address),
    UR_STR(zsrc_candidate_t, name),
    UR_STR(zsrc_candidate_t, detail),
    UR_STR(zsrc_candidate_t, state),
    UR_INT(zsrc_candidate_t, device_class),
    UR_BOOL(zsrc_candidate_t, changed),
    UR_BOOL(zsrc_candidate_t, best),
    UR_BOOL(zsrc_candidate_t, unusable),
    UR_INT(zsrc_candidate_t, zone),
    UR_BOOL(zsrc_candidate_t, eol),
};
const ur_table_t ur_t_candidate = UR_TABLE(zsrc_candidate_t, f_candidate);

static const ur_field_t f_source[] = {
    UR_INT(ui_zone_source_t, kind),
    UR_INT(ui_zone_source_t, caps),
    UR_BOOL(ui_zone_source_t, usable),
};
const ur_table_t ur_t_source = UR_TABLE(ui_zone_source_t, f_source);

/* The radios' (BL-223 step 2): made from ui_backend.h's structs in their fields' order, then
 * appended to only. A pointer (ui_out_status_t.where) is carried by hand in ur_serve.c. */
static const ur_field_t f_zw_status[] = {
    UR_INT(ui_zw_status_t, radio),
    UR_BOOL(ui_zw_status_t, update_advised),
    UR_BOOL(ui_zw_status_t, region_known),
    UR_INT(ui_zw_status_t, region),
    UR_INT(ui_zw_status_t, region_count),
    UR_INTS(ui_zw_status_t, regions),
    UR_INT(ui_zw_status_t, devices),
    UR_INT(ui_zw_status_t, not_heard),
    UR_INT(ui_zw_status_t, radio_fw_check),
    UR_STR(ui_zw_status_t, radio_fw_version),
    UR_STR(ui_zw_status_t, radio_fw_words),
    UR_INT(ui_zw_status_t, replaced),
    UR_INT(ui_zw_status_t, check_in_s),
    UR_INT(ui_zw_status_t, firmware_major),
    UR_INT(ui_zw_status_t, firmware_minor),
    UR_INT(ui_zw_status_t, sdk_major),
    UR_INT(ui_zw_status_t, sdk_minor),
    UR_INT(ui_zw_status_t, sdk_patch),
    UR_INT(ui_zw_status_t, home_id),
    UR_BOOL(ui_zw_status_t, noise_known),
    UR_INT(ui_zw_status_t, noise_dbm),
    UR_INT(ui_zw_status_t, joining),
    UR_INT(ui_zw_status_t, routes_done),
    UR_INT(ui_zw_status_t, routes_total),
    UR_BOOL(ui_zw_status_t, jammed),
    UR_INT(ui_zw_status_t, devices_lr),
    UR_INTS(ui_zw_status_t, region_change),
    UR_INTS(ui_zw_status_t, region_stop),
    UR_BOOL(ui_zw_status_t, storage_full),
};
const ur_table_t ur_t_zw_status = UR_TABLE(ui_zw_status_t, f_zw_status);
static const ur_field_t f_zw_device[] = {
    UR_INT(ui_zw_device_t, node),
    UR_INT(ui_zw_device_t, generation),
    UR_STR(ui_zw_device_t, model),
    UR_BOOL(ui_zw_device_t, has_identity),
    UR_INT(ui_zw_device_t, manufacturer),
    UR_INT(ui_zw_device_t, product_type),
    UR_INT(ui_zw_device_t, product_id),
    UR_INT(ui_zw_device_t, security),
    UR_INT(ui_zw_device_t, state),
    UR_BOOL(ui_zw_device_t, listening),
    UR_BOOL(ui_zw_device_t, flirs),
    UR_INT(ui_zw_device_t, power),
    UR_INT(ui_zw_device_t, setup_done),
    UR_INT(ui_zw_device_t, setup_total),
    UR_INT(ui_zw_device_t, kind),
    UR_BOOL(ui_zw_device_t, unreachable),
    UR_INT(ui_zw_device_t, battery_pct),
    UR_BOOL(ui_zw_device_t, battery_low),
    UR_INT(ui_zw_device_t, heard_s),
    UR_INT(ui_zw_device_t, signal),
    UR_INT(ui_zw_device_t, device_class),
    UR_INT(ui_zw_device_t, zone_count),
    UR_INTS(ui_zw_device_t, zones),
};
const ur_table_t ur_t_zw_device = UR_TABLE(ui_zw_device_t, f_zw_device);
static const ur_field_t f_zw_input[] = {
    UR_REC(ui_zw_input_t, addr, t_address),
    UR_INT(ui_zw_input_t, endpoint),
    UR_INT(ui_zw_input_t, slot),
    UR_INT(ui_zw_input_t, device_class),
    UR_STR(ui_zw_input_t, words),
    UR_STR(ui_zw_input_t, state),
    UR_INT(ui_zw_input_t, zone),
};
const ur_table_t ur_t_zw_input = UR_TABLE(ui_zw_input_t, f_zw_input);
static const ur_field_t f_zw_output[] = {
    UR_INT(ui_zw_output_t, endpoint),
    UR_INT(ui_zw_output_t, kind),
    UR_INT(ui_zw_output_t, level),
    UR_BOOL(ui_zw_output_t, busy),
    UR_INT(ui_zw_output_t, last),
    UR_BOOL(ui_zw_output_t, garage),
    UR_INT(ui_zw_output_t, garage_zone),
    UR_INT(ui_zw_output_t, garage_travel_s),
    UR_STR(ui_zw_output_t, garage_state),
    UR_INT(ui_zw_output_t, garage_goal),
};
const ur_table_t ur_t_zw_output = UR_TABLE(ui_zw_output_t, f_zw_output);
static const ur_field_t f_zw_device_page[] = {
    UR_REC(ui_zw_device_page_t, dev, ur_t_zw_device),
    UR_INT(ui_zw_device_page_t, check_in_s),
    UR_BOOL(ui_zw_device_page_t, check_in_known),
    UR_INT(ui_zw_device_page_t, check_in_next_s),
    UR_INT(ui_zw_device_page_t, check_in_min_s),
    UR_INT(ui_zw_device_page_t, check_in_max_s),
    UR_BOOL(ui_zw_device_page_t, can_identify),
    UR_STR(ui_zw_device_page_t, wake_words),
    UR_STR(ui_zw_device_page_t, add_words),
    UR_STR(ui_zw_device_page_t, note),
    UR_INT(ui_zw_device_page_t, input_count),
    UR_RECS(ui_zw_device_page_t, inputs, ur_t_zw_input),
    UR_INT(ui_zw_device_page_t, feedback.fit),
    UR_INT(ui_zw_device_page_t, feedback.partition),
    UR_BOOL(ui_zw_device_page_t, feedback.live),
    UR_INT(ui_zw_device_page_t, control.fit),
    UR_INT(ui_zw_device_page_t, control.partition),
    UR_INT(ui_zw_device_page_t, control.panic_mask),
    UR_BOOL(ui_zw_device_page_t, control.live),
    UR_INT(ui_zw_device_page_t, output_count),
    UR_RECS(ui_zw_device_page_t, outputs, ur_t_zw_output),
    UR_BOOL(ui_zw_device_page_t, fw.known),
    UR_INT(ui_zw_device_page_t, fw.major),
    UR_INT(ui_zw_device_page_t, fw.minor),
    UR_BOOL(ui_zw_device_page_t, fw.has_hardware),
    UR_INT(ui_zw_device_page_t, fw.hardware),
    UR_BOOL(ui_zw_device_page_t, fw.updatable),
    UR_INT(ui_zw_device_page_t, fw.check),
    UR_STR(ui_zw_device_page_t, fw.version),
    UR_BOOL(ui_zw_device_page_t, fw.beta),
    UR_STR(ui_zw_device_page_t, fw.words),
    UR_BOOL(ui_zw_device_page_t, identify),
    UR_INT(ui_zw_device_page_t, siren.fit),
    UR_INT(ui_zw_device_page_t, siren.endpoint),
    UR_BOOL(ui_zw_device_page_t, siren.tones),
    UR_INT(ui_zw_device_page_t, siren.partitions),
    UR_INT(ui_zw_device_page_t, siren.also),
    UR_BOOL(ui_zw_device_page_t, siren.burglary),
    UR_BOOL(ui_zw_device_page_t, siren.fire),
    UR_BOOL(ui_zw_device_page_t, siren.squawk),
    UR_INT(ui_zw_device_page_t, siren.volume),
    UR_INT(ui_zw_device_page_t, siren.supervision_min),
    UR_BOOL(ui_zw_device_page_t, siren.trouble),
    UR_BOOL(ui_zw_device_page_t, siren.sounding),
    UR_INT(ui_zw_device_page_t, jam_report_s),
};
const ur_table_t ur_t_zw_device_page = UR_TABLE(ui_zw_device_page_t, f_zw_device_page);
static const ur_field_t f_zw_request[] = {
    UR_INT(ui_zw_request_t, kind),
    UR_INT(ui_zw_request_t, user),
    UR_INT(ui_zw_request_t, node),
    UR_INT(ui_zw_request_t, generation),
    UR_INT(ui_zw_request_t, keys),
    UR_INT(ui_zw_request_t, region),
    UR_INT(ui_zw_request_t, pin),
    UR_INT(ui_zw_request_t, seconds),
    UR_INT(ui_zw_request_t, partition),
    UR_INT(ui_zw_request_t, panic_mask),
    UR_INT(ui_zw_request_t, endpoint),
    UR_INT(ui_zw_request_t, level),
    UR_INT(ui_zw_request_t, zone),
    UR_INT(ui_zw_request_t, parts),
    UR_INT(ui_zw_request_t, also),
    UR_INT(ui_zw_request_t, siren_flags),
    UR_INT(ui_zw_request_t, volume),
    UR_INT(ui_zw_request_t, minutes),
};
const ur_table_t ur_t_zw_request = UR_TABLE(ui_zw_request_t, f_zw_request);
static const ur_field_t f_zw_progress_routes_list[] = {
    {(uint16_t)(offsetof(ui_zw_progress_t, routes_list[0].node) - offsetof(ui_zw_progress_t, routes_list[0])), (uint16_t)UR_MEMBER_SIZE(ui_zw_progress_t, routes_list[0].node), UR_F_INT, 1u, NULL},
    {(uint16_t)(offsetof(ui_zw_progress_t, routes_list[0].what) - offsetof(ui_zw_progress_t, routes_list[0])), (uint16_t)UR_MEMBER_SIZE(ui_zw_progress_t, routes_list[0].what), UR_F_INT, 1u, NULL},
};
static const ur_table_t ur_t_zw_progress_routes_list = {f_zw_progress_routes_list, sizeof(f_zw_progress_routes_list) / sizeof(f_zw_progress_routes_list[0]), UR_MEMBER_SIZE(ui_zw_progress_t, routes_list[0])};
static const ur_field_t f_zw_progress[] = {
    UR_INT(ui_zw_progress_t, op),
    UR_INT(ui_zw_progress_t, step),
    UR_INT(ui_zw_progress_t, result),
    UR_INT(ui_zw_progress_t, seconds_left),
    UR_INT(ui_zw_progress_t, requested),
    UR_BOOL(ui_zw_progress_t, csa),
    UR_SECRET(ui_zw_progress_t, dsk),
    UR_INT(ui_zw_progress_t, node),
    UR_INT(ui_zw_progress_t, generation),
    UR_INT(ui_zw_progress_t, security),
    UR_INT(ui_zw_progress_t, s2_failure),
    UR_INT(ui_zw_progress_t, setup_done),
    UR_INT(ui_zw_progress_t, setup_total),
    UR_BOOL(ui_zw_progress_t, asleep),
    UR_BOOL(ui_zw_progress_t, stalled),
    UR_STR(ui_zw_progress_t, model),
    UR_INT(ui_zw_progress_t, zone_count),
    UR_INTS(ui_zw_progress_t, zones),
    UR_INT(ui_zw_progress_t, fw_done),
    UR_INT(ui_zw_progress_t, fw_total),
    UR_BOOL(ui_zw_progress_t, fw_wake),
    UR_INT(ui_zw_progress_t, fw_why),
    UR_INT(ui_zw_progress_t, fw_wait_s),
    UR_INT(ui_zw_progress_t, fw_error),
    UR_BOOL(ui_zw_progress_t, prov_forgot),
    UR_INT(ui_zw_progress_t, routes_done),
    UR_INT(ui_zw_progress_t, routes_total),
    UR_INT(ui_zw_progress_t, routes_failed),
    UR_INT(ui_zw_progress_t, routes_skipped),
    UR_INT(ui_zw_progress_t, routes_waiting),
    UR_BOOL(ui_zw_progress_t, routes_stopped),
    UR_INT(ui_zw_progress_t, routes_list_n),
    UR_RECS(ui_zw_progress_t, routes_list, ur_t_zw_progress_routes_list),
    UR_BOOL(ui_zw_progress_t, replacing),
};
const ur_table_t ur_t_zw_progress = UR_TABLE(ui_zw_progress_t, f_zw_progress);
static const ur_field_t f_zw_prov[] = {
    UR_INTS(ui_zw_prov_t, dsk),
    UR_STR(ui_zw_prov_t, name),
    UR_INT(ui_zw_prov_t, state),
    UR_INT(ui_zw_prov_t, failures),
    UR_BOOL(ui_zw_prov_t, long_range),
    UR_INT(ui_zw_prov_t, requested),
    UR_INT(ui_zw_prov_t, grant),
    UR_INT(ui_zw_prov_t, node),
    UR_INT(ui_zw_prov_t, generation),
    UR_INT(ui_zw_prov_t, generic),
    UR_INT(ui_zw_prov_t, manufacturer),
    UR_INT(ui_zw_prov_t, product_type),
    UR_INT(ui_zw_prov_t, product_id),
    UR_STR(ui_zw_prov_t, model),
};
const ur_table_t ur_t_zw_prov = UR_TABLE(ui_zw_prov_t, f_zw_prov);
static const ur_field_t f_zb_status[] = {
    UR_INT(ui_zb_status_t, radio),
    UR_INT(ui_zb_status_t, paused_why),
    UR_BOOL(ui_zb_status_t, jammed),
    UR_INT(ui_zb_status_t, devices),
    UR_INT(ui_zb_status_t, not_heard),
    UR_INT(ui_zb_status_t, codes_waiting),
    UR_INT(ui_zb_status_t, learn_s),
    UR_BOOL(ui_zb_status_t, network_known),
    UR_INT(ui_zb_status_t, channel),
    UR_INT(ui_zb_status_t, pan_id),
    UR_INT(ui_zb_status_t, ext_pan_id),
    UR_INT(ui_zb_status_t, panel_ieee),
    UR_STR(ui_zb_status_t, radio_version),
    UR_BOOL(ui_zb_status_t, noise_known),
    UR_INT(ui_zb_status_t, noise_dbm),
    UR_INT(ui_zb_status_t, survey_known),
    UR_INTS(ui_zb_status_t, survey_dbm),
    UR_INT(ui_zb_status_t, quietest),
    UR_INT(ui_zb_status_t, restarts),
    UR_BOOL(ui_zb_status_t, storage_full),
};
const ur_table_t ur_t_zb_status = UR_TABLE(ui_zb_status_t, f_zb_status);
static const ur_field_t f_zb_device[] = {
    UR_INT(ui_zb_device_t, id),
    UR_STR(ui_zb_device_t, name),
    UR_STR(ui_zb_device_t, model),
    UR_INT(ui_zb_device_t, state),
    UR_INT(ui_zb_device_t, kind),
    UR_INT(ui_zb_device_t, power),
    UR_BOOL(ui_zb_device_t, router),
    UR_INT(ui_zb_device_t, battery_pct),
    UR_BOOL(ui_zb_device_t, battery_low),
    UR_INT(ui_zb_device_t, heard_s),
    UR_INT(ui_zb_device_t, signal),
    UR_BOOL(ui_zb_device_t, unreachable),
    UR_INT(ui_zb_device_t, device_class),
    UR_INT(ui_zb_device_t, join),
    UR_INT(ui_zb_device_t, setup_done),
    UR_INT(ui_zb_device_t, setup_total),
    UR_INT(ui_zb_device_t, zone_count),
    UR_INTS(ui_zb_device_t, zones),
};
const ur_table_t ur_t_zb_device = UR_TABLE(ui_zb_device_t, f_zb_device);
static const ur_field_t f_zb_input[] = {
    UR_REC(ui_zb_input_t, addr, t_address),
    UR_INT(ui_zb_input_t, endpoint),
    UR_INT(ui_zb_input_t, device_class),
    UR_STR(ui_zb_input_t, words),
    UR_STR(ui_zb_input_t, state),
    UR_INT(ui_zb_input_t, zone),
};
const ur_table_t ur_t_zb_input = UR_TABLE(ui_zb_input_t, f_zb_input);
static const ur_field_t f_zb_siren[] = {
    UR_INT(ui_zb_siren_t, fit),
    UR_INT(ui_zb_siren_t, partitions),
    UR_BOOL(ui_zb_siren_t, burglary),
    UR_BOOL(ui_zb_siren_t, fire),
    UR_BOOL(ui_zb_siren_t, strobe),
    UR_BOOL(ui_zb_siren_t, squawk),
    UR_INT(ui_zb_siren_t, volume),
    UR_INT(ui_zb_siren_t, device),
    UR_BOOL(ui_zb_siren_t, live),
    UR_INT(ui_zb_siren_t, last_test),
    UR_INT(ui_zb_siren_t, supervision_min),
    UR_INT(ui_zb_siren_t, also),
};
const ur_table_t ur_t_zb_siren = UR_TABLE(ui_zb_siren_t, f_zb_siren);
static const ur_field_t f_zb_keypad[] = {
    UR_INT(ui_zb_keypad_t, fit),
    UR_INT(ui_zb_keypad_t, partition),
    UR_INT(ui_zb_keypad_t, panic_mask),
    UR_BOOL(ui_zb_keypad_t, show_state),
    UR_INT(ui_zb_keypad_t, device),
    UR_BOOL(ui_zb_keypad_t, live),
};
const ur_table_t ur_t_zb_keypad = UR_TABLE(ui_zb_keypad_t, f_zb_keypad);
static const ur_field_t f_zb_fob[] = {
    UR_INT(ui_zb_fob_t, fit),
    UR_INT(ui_zb_fob_t, user),
    UR_INT(ui_zb_fob_t, partition),
    UR_BOOL(ui_zb_fob_t, may_disarm),
    UR_BOOL(ui_zb_fob_t, panic),
    UR_BOOL(ui_zb_fob_t, live),
};
const ur_table_t ur_t_zb_fob = UR_TABLE(ui_zb_fob_t, f_zb_fob);
static const ur_field_t f_zb_device_page[] = {
    UR_REC(ui_zb_device_page_t, dev, ur_t_zb_device),
    UR_INT(ui_zb_device_page_t, ieee),
    UR_INT(ui_zb_device_page_t, nwk),
    UR_STR(ui_zb_device_page_t, manufacturer),
    UR_STR(ui_zb_device_page_t, model_id),
    UR_STR(ui_zb_device_page_t, sw_build),
    UR_INT(ui_zb_device_page_t, lqi),
    UR_INT(ui_zb_device_page_t, rssi),
    UR_INT(ui_zb_device_page_t, joined_at),
    UR_BOOL(ui_zb_device_page_t, sleepy),
    UR_INT(ui_zb_device_page_t, check_in_s),
    UR_INT(ui_zb_device_page_t, setup_step),
    UR_BOOL(ui_zb_device_page_t, can_identify),
    UR_STR(ui_zb_device_page_t, wake_words),
    UR_STR(ui_zb_device_page_t, add_words),
    UR_STR(ui_zb_device_page_t, note),
    UR_INT(ui_zb_device_page_t, input_count),
    UR_RECS(ui_zb_device_page_t, inputs, ur_t_zb_input),
    UR_REC(ui_zb_device_page_t, siren, ur_t_zb_siren),
    UR_REC(ui_zb_device_page_t, keypad, ur_t_zb_keypad),
    UR_REC(ui_zb_device_page_t, fob, ur_t_zb_fob),
};
const ur_table_t ur_t_zb_device_page = UR_TABLE(ui_zb_device_page_t, f_zb_device_page);
static const ur_field_t f_zb_request[] = {
    UR_INT(ui_zb_request_t, kind),
    UR_INT(ui_zb_request_t, user),
    UR_INT(ui_zb_request_t, via),
    UR_INT(ui_zb_request_t, id),
    UR_INT(ui_zb_request_t, ieee),
    UR_SECRET(ui_zb_request_t, code),
    UR_STR(ui_zb_request_t, name),
    UR_INT(ui_zb_request_t, channel),
    UR_REC(ui_zb_request_t, siren, ur_t_zb_siren),
    UR_REC(ui_zb_request_t, keypad, ur_t_zb_keypad),
    UR_REC(ui_zb_request_t, fob, ur_t_zb_fob),
    UR_SECRET(ui_zb_request_t, installer_code),
};
const ur_table_t ur_t_zb_request = UR_TABLE(ui_zb_request_t, f_zb_request);
static const ur_field_t f_zb_joined[] = {
    UR_INT(ui_zb_joined_t, id),
    UR_INT(ui_zb_joined_t, step),
    UR_INT(ui_zb_joined_t, result),
    UR_BOOL(ui_zb_joined_t, asleep),
    UR_STR(ui_zb_joined_t, model),
};
const ur_table_t ur_t_zb_joined = UR_TABLE(ui_zb_joined_t, f_zb_joined);
static const ur_field_t f_zb_progress[] = {
    UR_INT(ui_zb_progress_t, op),
    UR_INT(ui_zb_progress_t, step),
    UR_INT(ui_zb_progress_t, result),
    UR_INT(ui_zb_progress_t, seconds_left),
    UR_INT(ui_zb_progress_t, id),
    UR_INT(ui_zb_progress_t, old_id),
    UR_STR(ui_zb_progress_t, model),
    UR_INT(ui_zb_progress_t, joined_count),
    UR_RECS(ui_zb_progress_t, joined, ur_t_zb_joined),
    UR_INT(ui_zb_progress_t, zone_count),
    UR_INTS(ui_zb_progress_t, zones),
};
const ur_table_t ur_t_zb_progress = UR_TABLE(ui_zb_progress_t, f_zb_progress);
static const ur_field_t f_zb_code[] = {
    UR_INT(ui_zb_code_t, ieee),
    UR_INT(ui_zb_code_t, added_at),
};
const ur_table_t ur_t_zb_code = UR_TABLE(ui_zb_code_t, f_zb_code);
static const ur_field_t f_out_status[] = {
    UR_BOOL(ui_out_status_t, wired),
    /* where: a pointer, carried by hand (ur_proxy.c) */
    UR_INT(ui_out_status_t, rf_receiver),
    UR_BOOL(ui_out_status_t, rf_transmitter),
    UR_INT(ui_out_status_t, rf_heard_ms),
    UR_INT(ui_out_status_t, now_ms),
    UR_BOOL(ui_out_status_t, trouble),
};
const ur_table_t ur_t_out_status = UR_TABLE(ui_out_status_t, f_out_status);
static const ur_field_t f_out_wired[] = {
    UR_INT(ui_out_wired_t, function),
    UR_INT(ui_out_wired_t, gpio),
    UR_INT(ui_out_wired_t, sense),
    UR_INT(ui_out_wired_t, partitions),
    UR_INT(ui_out_wired_t, also),
    UR_BOOL(ui_out_wired_t, burglary),
    UR_BOOL(ui_out_wired_t, fire),
    UR_BOOL(ui_out_wired_t, squawk),
    UR_BOOL(ui_out_wired_t, active_low),
    UR_BOOL(ui_out_wired_t, on),
    UR_BOOL(ui_out_wired_t, trouble),
    UR_STR(ui_out_wired_t, name), /* an AUTOMATION output's (BL-224) */
};
const ur_table_t ur_t_out_wired = UR_TABLE(ui_out_wired_t, f_out_wired);
static const ur_field_t f_out_rf[] = {
    UR_BOOL(ui_out_rf_t, used),
    UR_INT(ui_out_rf_t, address),
    UR_INT(ui_out_rf_t, partitions),
    UR_INT(ui_out_rf_t, also),
    UR_BOOL(ui_out_rf_t, burglary),
    UR_BOOL(ui_out_rf_t, fire),
    UR_BOOL(ui_out_rf_t, squawk),
    UR_BOOL(ui_out_rf_t, sounding),
};
const ur_table_t ur_t_out_rf = UR_TABLE(ui_out_rf_t, f_out_rf);

size_t ui_remote_enc_binding(const zone_hub_binding_t *b, uint8_t *out, size_t cap)
{
    plink_w_t w;
    plink_w_init(&w, out, cap);
    ur_put(&w, &ur_t_binding, b);
    return w.ok ? w.len : 0u;
}

bool ui_remote_dec_binding(const uint8_t *p, size_t len, zone_hub_binding_t *b)
{
    plink_r_t r;
    plink_r_init(&r, p, len);
    ur_get(&r, &ur_t_binding, b);
    return r.ok;
}
