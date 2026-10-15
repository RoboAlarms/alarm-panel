/*
 * zw_cc_basic.h - No Operation, Basic, Binary Sensor, Multilevel Sensor and Battery
 * (command-classes.md §4.1, §4.2, §4.3, §4.4, §4.6): the plainest zone-facing CCs.
 */
#ifndef ZW_CC_BASIC_H
#define ZW_CC_BASIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc.h" /* zw_cc_t (ZW_CC_*); safe if we are already inside it (guards) */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- §4.1 No Operation (00): the whole command is the single byte 00, no command ID of its
 * own. Sent to a listening node as a ping; never to a sleeping one. ------------------------- */
size_t zw_cc_nop_build(uint8_t *out, size_t cap);
bool zw_cc_nop_parse(const uint8_t *p, size_t len);

/* ---- §4.2 Basic (20) ------------------------------------------------------------------------ */
#define ZW_CMD_BASIC_SET    0x01u
#define ZW_CMD_BASIC_GET    0x02u
#define ZW_CMD_BASIC_REPORT 0x03u

typedef struct {
    uint8_t value; /* 00 off/idle, FF or 01..63 on/active */
} zw_cc_basic_set_t;
size_t zw_cc_basic_set_build(uint8_t *out, size_t cap, const zw_cc_basic_set_t *in);
bool zw_cc_basic_set_parse(const uint8_t *p, size_t len, zw_cc_basic_set_t *out);

size_t zw_cc_basic_get_build(uint8_t *out, size_t cap);
bool zw_cc_basic_get_parse(const uint8_t *p, size_t len);

/* v2 adds target_value/duration once the command is long enough to carry them. */
typedef struct {
    uint8_t current_value;
    bool has_target;
    uint8_t target_value;
    uint8_t duration;
} zw_cc_basic_report_t;
size_t zw_cc_basic_report_build(uint8_t *out, size_t cap, const zw_cc_basic_report_t *in);
bool zw_cc_basic_report_parse(const uint8_t *p, size_t len, zw_cc_basic_report_t *out);

/* ---- §4.3 Binary Sensor (30) ------------------------------------------------------------------ */
#define ZW_CMD_BINARY_SENSOR_SUPPORTED_GET    0x01u
#define ZW_CMD_BINARY_SENSOR_GET              0x02u
#define ZW_CMD_BINARY_SENSOR_REPORT           0x03u
#define ZW_CMD_BINARY_SENSOR_SUPPORTED_REPORT 0x04u

#define ZW_BINARY_SENSOR_TYPE_GENERAL     0x01u
#define ZW_BINARY_SENSOR_TYPE_SMOKE       0x02u
#define ZW_BINARY_SENSOR_TYPE_CO          0x03u
#define ZW_BINARY_SENSOR_TYPE_CO2         0x04u
#define ZW_BINARY_SENSOR_TYPE_HEAT        0x05u
#define ZW_BINARY_SENSOR_TYPE_WATER       0x06u
#define ZW_BINARY_SENSOR_TYPE_FREEZE      0x07u
#define ZW_BINARY_SENSOR_TYPE_TAMPER      0x08u
#define ZW_BINARY_SENSOR_TYPE_AUX         0x09u
#define ZW_BINARY_SENSOR_TYPE_DOOR_WINDOW 0x0Au
#define ZW_BINARY_SENSOR_TYPE_TILT        0x0Bu
#define ZW_BINARY_SENSOR_TYPE_MOTION      0x0Cu
#define ZW_BINARY_SENSOR_TYPE_GLASS_BREAK 0x0Du
#define ZW_BINARY_SENSOR_TYPE_DEFAULT     0xFFu /* Get only: "the node's default" */

size_t zw_cc_binary_sensor_supported_get_build(uint8_t *out, size_t cap);
bool zw_cc_binary_sensor_supported_get_parse(const uint8_t *p, size_t len);

/* bit 0 of byte 0 is reserved; bit n (counting across bytes from there) = sensor type n. */
typedef struct {
    const uint8_t *mask;
    size_t mask_len;
} zw_cc_binary_sensor_supported_report_t;
size_t zw_cc_binary_sensor_supported_report_build(uint8_t *out, size_t cap,
                                                   const zw_cc_binary_sensor_supported_report_t *in);
bool zw_cc_binary_sensor_supported_report_parse(const uint8_t *p, size_t len,
                                                 zw_cc_binary_sensor_supported_report_t *out);
bool zw_cc_binary_sensor_type_supported(const uint8_t *mask, size_t mask_len, uint8_t sensor_type);

typedef struct {
    bool has_type; /* v2 */
    uint8_t sensor_type;
} zw_cc_binary_sensor_get_t;
size_t zw_cc_binary_sensor_get_build(uint8_t *out, size_t cap, const zw_cc_binary_sensor_get_t *in);
bool zw_cc_binary_sensor_get_parse(const uint8_t *p, size_t len, zw_cc_binary_sensor_get_t *out);

typedef struct {
    bool detected; /* the value byte, strictly 00 or FF */
    bool has_type; /* v2 */
    uint8_t sensor_type;
} zw_cc_binary_sensor_report_t;
size_t zw_cc_binary_sensor_report_build(uint8_t *out, size_t cap, const zw_cc_binary_sensor_report_t *in);
bool zw_cc_binary_sensor_report_parse(const uint8_t *p, size_t len, zw_cc_binary_sensor_report_t *out);

/* ---- §4.4 Multilevel Sensor (31) --------------------------------------------------------------- */
#define ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_GET    0x01u
#define ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SENSOR_REPORT 0x02u
#define ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_GET     0x03u
#define ZW_CMD_MULTILEVEL_SENSOR_GET                     0x04u
#define ZW_CMD_MULTILEVEL_SENSOR_REPORT                  0x05u
#define ZW_CMD_MULTILEVEL_SENSOR_SUPPORTED_SCALE_REPORT  0x06u

#define ZW_MULTILEVEL_SENSOR_TYPE_AIR_TEMPERATURE 0x01u
#define ZW_MULTILEVEL_SENSOR_TYPE_LUMINANCE       0x03u
#define ZW_MULTILEVEL_SENSOR_TYPE_HUMIDITY        0x05u
#define ZW_MULTILEVEL_SENSOR_SCALE_CELSIUS        0u
#define ZW_MULTILEVEL_SENSOR_SCALE_FAHRENHEIT     1u

size_t zw_cc_multilevel_sensor_supported_sensor_get_build(uint8_t *out, size_t cap);
bool zw_cc_multilevel_sensor_supported_sensor_get_parse(const uint8_t *p, size_t len);

/* bit 0 of byte 0 = sensor type 1 (one-based, unlike Binary Sensor's reserved bit 0). */
typedef struct {
    const uint8_t *mask;
    size_t mask_len;
} zw_cc_multilevel_sensor_supported_sensor_report_t;
size_t zw_cc_multilevel_sensor_supported_sensor_report_build(
    uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_supported_sensor_report_t *in);
bool zw_cc_multilevel_sensor_supported_sensor_report_parse(
    const uint8_t *p, size_t len, zw_cc_multilevel_sensor_supported_sensor_report_t *out);
bool zw_cc_multilevel_sensor_type_supported(const uint8_t *mask, size_t mask_len, uint8_t sensor_type);

typedef struct {
    uint8_t sensor_type;
} zw_cc_multilevel_sensor_supported_scale_get_t;
size_t zw_cc_multilevel_sensor_supported_scale_get_build(
    uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_supported_scale_get_t *in);
bool zw_cc_multilevel_sensor_supported_scale_get_parse(
    const uint8_t *p, size_t len, zw_cc_multilevel_sensor_supported_scale_get_t *out);

typedef struct {
    uint8_t sensor_type;
    uint8_t scale_mask; /* bits 3..0: scale 0..3 supported */
} zw_cc_multilevel_sensor_supported_scale_report_t;
size_t zw_cc_multilevel_sensor_supported_scale_report_build(
    uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_supported_scale_report_t *in);
bool zw_cc_multilevel_sensor_supported_scale_report_parse(
    const uint8_t *p, size_t len, zw_cc_multilevel_sensor_supported_scale_report_t *out);

/* v5+ payload is sensor_type followed by a byte holding scale<<3 (bits 4..3, matching the
 * Report's own scale position); v1..v4 send neither. */
typedef struct {
    bool has_type;
    uint8_t sensor_type;
    uint8_t scale;
} zw_cc_multilevel_sensor_get_t;
size_t zw_cc_multilevel_sensor_get_build(uint8_t *out, size_t cap, const zw_cc_multilevel_sensor_get_t *in);
bool zw_cc_multilevel_sensor_get_parse(const uint8_t *p, size_t len, zw_cc_multilevel_sensor_get_t *out);

/* value = raw_value / 10^precision (an integer decode - never floating point; the caller divides
 * however it prefers, e.g. by whole part and remainder). size is 1, 2, 3 or 4 bytes, signed. */
typedef struct {
    uint8_t sensor_type;
    uint8_t precision;
    uint8_t scale;    /* 0..3 */
    uint8_t size;     /* 1, 2, 3 or 4 */
    int32_t raw_value;
} zw_cc_multilevel_sensor_report_t;
size_t zw_cc_multilevel_sensor_report_build(uint8_t *out, size_t cap,
                                             const zw_cc_multilevel_sensor_report_t *in);
bool zw_cc_multilevel_sensor_report_parse(const uint8_t *p, size_t len,
                                           zw_cc_multilevel_sensor_report_t *out);

/* ---- §4.6 Battery (80) --------------------------------------------------------------------------- */
#define ZW_CMD_BATTERY_GET    0x02u
#define ZW_CMD_BATTERY_REPORT 0x03u
#define ZW_BATTERY_LOW        0xFFu /* level: "battery low warning" instead of a percentage */

size_t zw_cc_battery_get_build(uint8_t *out, size_t cap);
bool zw_cc_battery_get_parse(const uint8_t *p, size_t len);

/* v2 flags1: bits 7..6 charging (00 discharging, 01 charging, 10 maintaining), bit 5
 * rechargeable, bit 4 backup battery, bit 3 overheating, bit 2 low fluid, bits 1..0 replace/
 * recharge (01 soon, 10 now). flags2: bit 1 low temperature, bit 0 disconnected. */
typedef struct {
    uint8_t level; /* 0..100, or ZW_BATTERY_LOW */
    bool has_v2;
    uint8_t charging_state;
    bool rechargeable;
    bool is_backup;
    bool overheating;
    bool low_fluid;
    uint8_t replace_or_recharge; /* 00 none, 01 soon, 10 now */
    bool low_temperature;
    bool disconnected;
} zw_cc_battery_report_t;
size_t zw_cc_battery_report_build(uint8_t *out, size_t cap, const zw_cc_battery_report_t *in);
bool zw_cc_battery_report_parse(const uint8_t *p, size_t len, zw_cc_battery_report_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZW_CC_BASIC_H */
