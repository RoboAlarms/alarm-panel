/* Entry Control v1 (6F). Buffer views contain credentials: never log or persist them.
 * Parsers validate lengths before returning views; callers own the input buffer. */
#ifndef ZW_CC_ENTRY_CONTROL_H
#define ZW_CC_ENTRY_CONTROL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define ZW_CMD_ENTRY_NOTIFICATION       0x01u
#define ZW_CMD_ENTRY_KEY_SUPPORTED_GET  0x02u
#define ZW_CMD_ENTRY_KEY_SUPPORTED_REPORT 0x03u
#define ZW_CMD_ENTRY_EVENT_SUPPORTED_GET 0x04u
#define ZW_CMD_ENTRY_EVENT_SUPPORTED_REPORT 0x05u
#define ZW_CMD_ENTRY_CONFIGURATION_SET  0x06u
#define ZW_CMD_ENTRY_CONFIGURATION_GET  0x07u
#define ZW_CMD_ENTRY_CONFIGURATION_REPORT 0x08u
#define ZW_ENTRY_DATA_NONE  0u
#define ZW_ENTRY_DATA_RAW   1u
#define ZW_ENTRY_DATA_ASCII 2u
#define ZW_ENTRY_DATA_MD5   3u
#define ZW_ENTRY_DATA_MAX   32u
#define ZW_ENTRY_EVENT_CACHING 0x00u
#define ZW_ENTRY_EVENT_CACHED_KEYS 0x01u
#define ZW_ENTRY_EVENT_ENTER 0x02u
#define ZW_ENTRY_EVENT_DISARM_ALL 0x03u
#define ZW_ENTRY_EVENT_ARM_AWAY 0x05u
#define ZW_ENTRY_EVENT_ARM_HOME 0x06u
#define ZW_ENTRY_EVENT_FIRE 0x10u
#define ZW_ENTRY_EVENT_POLICE 0x11u
#define ZW_ENTRY_EVENT_MEDICAL 0x13u

typedef struct {
    uint8_t sequence, data_type, event_type;
    const uint8_t *data;
    size_t data_len; /* wire bytes, including any trailing FF padding */
} zw_cc_entry_notification_t;
bool zw_cc_entry_notification_parse(const uint8_t *p, size_t len, zw_cc_entry_notification_t *out);
/* Pads ASCII to 16-byte blocks. Accepts already padded input too. */
size_t zw_cc_entry_notification_build(uint8_t *out, size_t cap, const zw_cc_entry_notification_t *in);

typedef struct { const uint8_t *mask; size_t mask_len; } zw_cc_entry_keys_t;
bool zw_cc_entry_keys_parse(const uint8_t *p, size_t len, zw_cc_entry_keys_t *out);
size_t zw_cc_entry_keys_build(uint8_t *out, size_t cap, const zw_cc_entry_keys_t *in);

typedef struct {
    const uint8_t *data_types, *event_types;
    size_t data_types_len, event_types_len;
    uint8_t min_cache, max_cache, min_timeout, max_timeout;
} zw_cc_entry_events_t;
bool zw_cc_entry_events_parse(const uint8_t *p, size_t len, zw_cc_entry_events_t *out);
size_t zw_cc_entry_events_build(uint8_t *out, size_t cap, const zw_cc_entry_events_t *in);

typedef struct { uint8_t cache_size, timeout_s; } zw_cc_entry_configuration_t;
bool zw_cc_entry_configuration_set_parse(const uint8_t *p, size_t len, zw_cc_entry_configuration_t *out);
bool zw_cc_entry_configuration_report_parse(const uint8_t *p, size_t len, zw_cc_entry_configuration_t *out);
size_t zw_cc_entry_configuration_set_build(uint8_t *out, size_t cap, const zw_cc_entry_configuration_t *in);
size_t zw_cc_entry_configuration_report_build(uint8_t *out, size_t cap, const zw_cc_entry_configuration_t *in);

bool zw_cc_entry_key_supported_get_parse(const uint8_t *p, size_t len);
bool zw_cc_entry_event_supported_get_parse(const uint8_t *p, size_t len);
bool zw_cc_entry_configuration_get_parse(const uint8_t *p, size_t len);
size_t zw_cc_entry_key_supported_get_build(uint8_t *out, size_t cap);
size_t zw_cc_entry_event_supported_get_build(uint8_t *out, size_t cap);
size_t zw_cc_entry_configuration_get_build(uint8_t *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
