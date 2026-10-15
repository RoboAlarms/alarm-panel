/*
 * proto_zw.h - the Z-Wave devices the panel switches, for Home Assistant over MQTT
 * (docs/features/10 MQTT-018..020, docs/features/36 ZW-036). Topics, keys and ids are an external
 * contract: never rename, only add.
 *
 * One entity an output: a switch or relay ("switch"), a dimmer ("light", JSON schema, brightness
 * 0..100), a garage door ("cover", device class garage). Its object id and unique_id carry the
 * network's home ID, the device's node ID and generation and the end point
 * ("zw_<home>_<node>_<generation>_<endpoint>"): the same across restarts, a new one when a node
 * ID is given to another device, so nothing is announced twice and a removed device's entity is
 * retracted by its id.
 *   state   ~/zwave/<key>/state      retained  {"state":"ON","brightness":40} | {"state":"open"}
 *   online  ~/zwave/<key>/available  retained  "online" | "offline" (with the panel's own)
 *   command ~/zwave/<key>/set        "ON" / "OFF"; a light's {"state":"ON","brightness":40};
 *                                    a garage's "CLOSE"
 * A garage door opens only at the panel, with a user's code (docs/features/24 HCTL-005): its
 * entity offers no open (payload_open null), nor stop. Unknown position is HA's unknown ("None").
 *
 * Portable C11, no heap.
 */
#ifndef PROTO_ZW_H
#define PROTO_ZW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "alarm_proto/proto.h"
#include "alarm_proto/proto_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_ZW_KEY_LEN 40

typedef enum {
    PROTO_ZW_SWITCH = 1,
    PROTO_ZW_LIGHT,
    PROTO_ZW_GARAGE,
} proto_zw_kind_t;

typedef struct {
    char key[PROTO_ZW_KEY_LEN];
    uint8_t kind;           /* proto_zw_kind_t */
    char name[48];
    int8_t level;           /* -1 unknown, 0..100 */
    bool online;
    char garage[12];        /* its sensor's word: "open", "closed", "unknown" (a press is never
                             * reported as "opening": a tilt sensor can't see a door on its way) */
} proto_zw_device_t;

/* "zw_0a1b2c3d_7_2_1". false: it didn't fit. */
bool proto_zw_key(uint32_t home_id, uint16_t node, uint16_t generation, uint8_t endpoint,
                  char out[PROTO_ZW_KEY_LEN]);
/* "switch", "light", "cover"; NULL for another kind. */
const char *proto_zw_component(uint8_t kind);
/* "<prefix>/<panel_id>/zwave/<key>/<leaf>". */
bool proto_zw_topic(const proto_ident_t *id, const char *key, const char *leaf, char *out,
                    size_t cap);

/* Its discovery document. */
void proto_ha_zw_device(pj_writer_t *w, const proto_ident_t *id, const proto_zw_device_t *d);
/* Its state payload. */
void proto_zw_state(pj_writer_t *w, const proto_zw_device_t *d);

/* A command for a device of this kind: the level to switch to (0..100), or a garage's close
 * (*level 0). ALARM_ERR_BAD_ARG: not a command this kind takes (a garage's OPEN included). */
alarm_result_t proto_zw_cmd_parse(const char *payload, size_t len, uint8_t kind, int *level);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_ZW_H */
